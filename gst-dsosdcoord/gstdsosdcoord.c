/**
 * Copyright (c) 2016-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA Corporation is strictly prohibited.
 *
 * version: 0.1
 */

#include <stdio.h>
#include <gst/gst.h>

#include <gst/video/video.h>
#include <gst/base/gstbasetransform.h>
#include "gstdsosdcoord.h"
#include <cuda.h>
#include <cuda_runtime.h>

#include "nvbufsurface.h"
#include "nvtx3/nvToolsExt.h"

GST_DEBUG_CATEGORY_STATIC (gst_ds_osdcoord_debug);
#define GST_CAT_DEFAULT gst_ds_osdcoord_debug

/* For hw blending, color should be of the form:
   class_id1, R, G, B, A:class_id2, R, G, B, A */
#define DEFAULT_CLR "0,0.0,1.0,0.0,0.3:1,0.0,1.0,1.0,0.3:2,0.0,0.0,1.0,0.3:3,1.0,1.0,0.0,0.3"
#define MAX_OSD_ELEMS 1024

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

/* Enum to identify properties */
enum
{
  PROP_0,
  PROP_SHOW_CLOCK,
  PROP_SHOW_TEXT,
  PROP_CLOCK_FONT,
  PROP_CLOCK_FONT_SIZE,
  PROP_CLOCK_X_OFFSET,
  PROP_CLOCK_Y_OFFSET,
  PROP_CLOCK_COLOR,
  PROP_PROCESS_MODE,
  PROP_HW_BLEND_COLOR_ATTRS,
  PROP_GPU_DEVICE_ID,
  PROP_SHOW_BBOX,
  PROP_SHOW_MASK,
  PROP_SHOW_COORD,
};

/* the capabilities of the inputs and outputs. */
static GstStaticPadTemplate dsosdcoord_sink_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ RGBA }")));

static GstStaticPadTemplate dsosdcoord_src_factory =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_NVMM,
            "{ RGBA }")));

/* Default values for properties */
#define DEFAULT_FONT_SIZE 12
#define DEFAULT_FONT "Serif"
#ifdef PLATFORM_TEGRA
#define GST_NV_OSD_DEFAULT_PROCESS_MODE MODE_HW
#else
#define GST_NV_OSD_DEFAULT_PROCESS_MODE MODE_GPU
#endif
#define MAX_FONT_SIZE 60
#define DEFAULT_BORDER_WIDTH 4

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_ds_osdcoord_parent_class parent_class
G_DEFINE_TYPE (GstDsOsdCoord, gst_ds_osdcoord, GST_TYPE_BASE_TRANSFORM);

#define GST_TYPE_NV_OSD_PROCESS_MODE (gst_ds_osdcoord_process_mode_get_type ())

static GQuark _dsmeta_quark;

static GType
gst_ds_osdcoord_process_mode_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {MODE_CPU, "CPU_MODE", "CPU_MODE"},
      {MODE_GPU, "GPU_MODE, yet to be implemented for Tegra", "GPU_MODE"},
#ifdef PLATFORM_TEGRA
      {MODE_HW,
            "HW_MODE. Only for Tegra. For rectdraw only.",
          "HW_MODE"},
#endif
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstDsOsdCoordMode", values);
  }
  return qtype;
}

static void gst_ds_osdcoord_finalize (GObject * object);
static void gst_ds_osdcoord_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_ds_osdcoord_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstFlowReturn gst_ds_osdcoord_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);
static gboolean gst_ds_osdcoord_start (GstBaseTransform * btrans);
static gboolean gst_ds_osdcoord_stop (GstBaseTransform * btrans);
static gboolean gst_ds_osdcoord_parse_color (GstDsOsdCoord * dsosdcoord,
    guint clock_color);

static gboolean gst_ds_osdcoord_parse_hw_blend_color_attrs (GstDsOsdCoord * dsosdcoord,
    const gchar * arr);
static gboolean gst_ds_osdcoord_get_hw_blend_color_attrs (GValue * value,
    GstDsOsdCoord * dsosdcoord);

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean
gst_ds_osdcoord_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  gboolean ret = TRUE;

  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (trans);
  gint width = 0, height = 0;
  cudaError_t CUerr = cudaSuccess;

  dsosdcoord->frame_num = 0;

  GstStructure *structure = gst_caps_get_structure (incaps, 0);

  GST_OBJECT_LOCK (dsosdcoord);
  if (!gst_structure_get_int (structure, "width", &width) ||
      !gst_structure_get_int (structure, "height", &height)) {
    GST_ELEMENT_ERROR (dsosdcoord, STREAM, FAILED,
        ("caps without width/height"), NULL);
    ret = FALSE;
    goto exit_set_caps;
  }
  if (dsosdcoord->dsosdcoord_context && dsosdcoord->width == width
      && dsosdcoord->height == height) {
    goto exit_set_caps;
  }

  CUerr = cudaSetDevice (dsosdcoord->gpu_id);
  if (CUerr != cudaSuccess) {
    ret = FALSE;
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    goto exit_set_caps;
  }

  dsosdcoord->width = width;
  dsosdcoord->height = height;

  if (dsosdcoord->show_clock)
    nvll_osd_set_clock_params (dsosdcoord->dsosdcoord_context,
        &dsosdcoord->clock_text_params);

  dsosdcoord->conv_buf =
      nvll_osd_set_params (dsosdcoord->dsosdcoord_context, dsosdcoord->width,
      dsosdcoord->height);

exit_set_caps:
  GST_OBJECT_UNLOCK (dsosdcoord);
  return ret;
}

/**
 * Initialize all resources.
 */
static gboolean
gst_ds_osdcoord_start (GstBaseTransform * btrans)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (btrans);

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoord->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return FALSE;
  }
  GST_LOG_OBJECT (dsosdcoord, "SETTING CUDA DEVICE = %d in dsosdcoord func=%s\n",
      dsosdcoord->gpu_id, __func__);

  dsosdcoord->dsosdcoord_context = nvll_osd_create_context ();

  if (dsosdcoord->dsosdcoord_context == NULL) {
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to create context dsosdcoord"), NULL);
    return FALSE;
  }

  int flag_integrated = -1;
  cudaDeviceGetAttribute(&flag_integrated, cudaDevAttrIntegrated, dsosdcoord->gpu_id);
  if(!flag_integrated && dsosdcoord->dsosdcoord_mode == MODE_HW) {
    dsosdcoord->dsosdcoord_mode = MODE_GPU;
  }

  if (dsosdcoord->num_class_entries == 0) {
    gst_ds_osdcoord_parse_hw_blend_color_attrs (dsosdcoord, DEFAULT_CLR);
  }

  nvll_osd_init_colors_for_hw_blend (dsosdcoord->dsosdcoord_context,
      dsosdcoord->color_info, dsosdcoord->num_class_entries);

  if (dsosdcoord->show_clock) {
    nvll_osd_set_clock_params (dsosdcoord->dsosdcoord_context,
        &dsosdcoord->clock_text_params);
  }

  return TRUE;
}

/**
 * Free up all the resources
 */
static gboolean
gst_ds_osdcoord_stop (GstBaseTransform * btrans)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (btrans);

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoord->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return FALSE;
  }
  GST_LOG_OBJECT (dsosdcoord, "SETTING CUDA DEVICE = %d in dsosdcoord func=%s\n",
      dsosdcoord->gpu_id, __func__);

  if (dsosdcoord->dsosdcoord_context)
    nvll_osd_destroy_context (dsosdcoord->dsosdcoord_context);

  dsosdcoord->dsosdcoord_context = NULL;
  dsosdcoord->width = 0;
  dsosdcoord->height = 0;

  return TRUE;
}

int frame_num = 0;

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn
gst_ds_osdcoord_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (trans);
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  unsigned int rect_cnt = 0;
  unsigned int segment_cnt = 0;
  unsigned int text_cnt = 0;
  unsigned int line_cnt = 0;
  unsigned int arrow_cnt = 0;
  unsigned int circle_cnt = 0;
  unsigned int i = 0;
  int idx = 0;
  typedef struct coord {
    double x;
    double y;
  } COORD;
  gpointer state = NULL;
  NvBufSurface *surface = NULL;
  NvDsBatchMeta *batch_meta = NULL;

  if (!gst_buffer_map (buf, &inmap, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to map info from buffer"), NULL);
    return GST_FLOW_ERROR;
  }

  nvds_set_input_system_timestamp (buf, GST_ELEMENT_NAME (dsosdcoord));

  cudaError_t CUerr = cudaSuccess;
  CUerr = cudaSetDevice (dsosdcoord->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
        ("Unable to set device"), NULL);
    return GST_FLOW_ERROR;
  }
  GST_LOG_OBJECT (dsosdcoord, "SETTING CUDA DEVICE = %d in dsosdcoord func=%s\n",
      dsosdcoord->gpu_id, __func__);

  surface = (NvBufSurface *) inmap.data;

  /* Get metadata. Update rectangle and text params */
  GstMeta *gst_meta;
  NvDsMeta *dsmeta;
  char context_name[100];
  snprintf (context_name, sizeof (context_name), "%s_(Frame=%u)",
      GST_ELEMENT_NAME (dsosdcoord), dsosdcoord->frame_num);
  nvtxRangePushA (context_name);
  while ((gst_meta = gst_buffer_iterate_meta (buf, &state))) {
    if (gst_meta_api_type_has_tag (gst_meta->info->api, _dsmeta_quark)) {
      dsmeta = (NvDsMeta *) gst_meta;
      if (dsmeta->meta_type == NVDS_BATCH_GST_META) {
        batch_meta = (NvDsBatchMeta *) dsmeta->meta_data;
        break;
      }
    }
  }

  NvDsMetaList *l = NULL;
  NvDsMetaList *full_obj_meta_list = NULL;
  if (batch_meta)
    full_obj_meta_list = batch_meta->obj_meta_pool->full_list;
  NvDsObjectMeta *object_meta = NULL;

  for (l = full_obj_meta_list; l != NULL; l = l->next) {
    object_meta = (NvDsObjectMeta *) (l->data);
    if (dsosdcoord->draw_bbox) {
      dsosdcoord->rect_params[rect_cnt] = object_meta->rect_params;
#ifdef PLATFORM_TEGRA
      /* In case of hardware blending, values set in hw-blend-color-attr
         should be considered as rect bg color values*/
      if (dsosdcoord->dsosdcoord_mode == MODE_HW && dsosdcoord->hw_blend) {
        for (idx = 0; idx < dsosdcoord->num_class_entries; idx++) {
          if (dsosdcoord->color_info[idx].id == object_meta->class_id) {
            dsosdcoord->rect_params[rect_cnt].color_id = idx;
            dsosdcoord->rect_params[rect_cnt].has_bg_color = TRUE;
            dsosdcoord->rect_params[rect_cnt].bg_color.red =
              dsosdcoord->color_info[idx].color.red;
            dsosdcoord->rect_params[rect_cnt].bg_color.blue =
              dsosdcoord->color_info[idx].color.blue;
            dsosdcoord->rect_params[rect_cnt].bg_color.green =
              dsosdcoord->color_info[idx].color.green;
            dsosdcoord->rect_params[rect_cnt].bg_color.alpha =
              dsosdcoord->color_info[idx].color.alpha;
            break;
          }
        }
      }
#endif
      rect_cnt++;
    }
    /* Display the label and coordinates of the drawn bboxs*/
    if (dsosdcoord->display_coord) {
      COORD top_left, bottom_right;
      top_left.x = object_meta->rect_params.left;
      top_left.y = object_meta->rect_params.top;
      bottom_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
      bottom_right.y = object_meta->rect_params.top + object_meta->rect_params.height;
      g_print("%u: %s, ", dsosdcoord->frame_num, object_meta->text_params.display_text);
      g_print ("Top Left: (%f, %f), Bottom Right: (%f, %f)\n", top_left.x, top_left.y, bottom_right.x, bottom_right.y);
    }

    if (rect_cnt == MAX_OSD_ELEMS) {
      dsosdcoord->frame_rect_params->num_rects = rect_cnt;
      dsosdcoord->frame_rect_params->rect_params_list = dsosdcoord->rect_params;
      dsosdcoord->frame_rect_params->buf_ptr = &surface->surfaceList[0];
      dsosdcoord->frame_rect_params->mode = dsosdcoord->dsosdcoord_mode;
      if (nvll_osd_draw_rectangles (dsosdcoord->dsosdcoord_context,
              dsosdcoord->frame_rect_params) == -1) {
        GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
            ("Unable to draw rectangles"), NULL);
        return GST_FLOW_ERROR;
      }
      rect_cnt = 0;
    }
    if (dsosdcoord->draw_mask && object_meta->mask_params.data &&
                              object_meta->mask_params.size > 0) {
      dsosdcoord->mask_rect_params[segment_cnt] = object_meta->rect_params;
      dsosdcoord->mask_params[segment_cnt++] = object_meta->mask_params;
      if (segment_cnt == MAX_OSD_ELEMS) {
        dsosdcoord->frame_mask_params->num_segments = segment_cnt;
        dsosdcoord->frame_mask_params->rect_params_list = dsosdcoord->mask_rect_params;
        dsosdcoord->frame_mask_params->mask_params_list = dsosdcoord->mask_params;
        dsosdcoord->frame_mask_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoord->frame_mask_params->mode = dsosdcoord->dsosdcoord_mode;
        if (nvll_osd_draw_segment_masks (dsosdcoord->dsosdcoord_context,
                dsosdcoord->frame_mask_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
              ("Unable to draw rectangles"), NULL);
          return GST_FLOW_ERROR;
        }
        segment_cnt = 0;
      }
    }
    if (object_meta->text_params.display_text)
      dsosdcoord->text_params[text_cnt++] = object_meta->text_params;
    if (text_cnt == MAX_OSD_ELEMS) {
      dsosdcoord->frame_text_params->num_strings = text_cnt;
      dsosdcoord->frame_text_params->text_params_list = dsosdcoord->text_params;
      dsosdcoord->frame_text_params->buf_ptr = &surface->surfaceList[0];
      dsosdcoord->frame_text_params->mode = dsosdcoord->dsosdcoord_mode;
      if (nvll_osd_put_text (dsosdcoord->dsosdcoord_context,
              dsosdcoord->frame_text_params) == -1) {
        GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
            ("Unable to draw text"), NULL);
        return GST_FLOW_ERROR;
      }
      text_cnt = 0;
    }
  }

  NvDsMetaList *display_meta_list = NULL;
  if (batch_meta)
    display_meta_list = batch_meta->display_meta_pool->full_list;
  NvDsDisplayMeta *display_meta = NULL;

  /* Get objects to be drawn from display meta.
   * Draw objects if count equals MAX_OSD_ELEMS.
   */
  for (l = display_meta_list; l != NULL; l = l->next) {
    display_meta = (NvDsDisplayMeta *) (l->data);

    unsigned int cnt = 0;
    for (cnt = 0; cnt < display_meta->num_rects; cnt++) {
      dsosdcoord->rect_params[rect_cnt++] = display_meta->rect_params[cnt];
      if (rect_cnt == MAX_OSD_ELEMS) {
        dsosdcoord->frame_rect_params->num_rects = rect_cnt;
        dsosdcoord->frame_rect_params->rect_params_list = dsosdcoord->rect_params;
        dsosdcoord->frame_rect_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoord->frame_rect_params->mode = dsosdcoord->dsosdcoord_mode;
        if (nvll_osd_draw_rectangles (dsosdcoord->dsosdcoord_context,
                dsosdcoord->frame_rect_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
              ("Unable to draw rectangles"), NULL);
          return GST_FLOW_ERROR;
        }
        rect_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_labels; cnt++) {
      if (display_meta->text_params[cnt].display_text) {
        dsosdcoord->text_params[text_cnt++] = display_meta->text_params[cnt];
        if (text_cnt == MAX_OSD_ELEMS) {
          dsosdcoord->frame_text_params->num_strings = text_cnt;
          dsosdcoord->frame_text_params->text_params_list = dsosdcoord->text_params;
          dsosdcoord->frame_text_params->buf_ptr = &surface->surfaceList[0];
          dsosdcoord->frame_text_params->mode = dsosdcoord->dsosdcoord_mode;
          if (nvll_osd_put_text (dsosdcoord->dsosdcoord_context,
                  dsosdcoord->frame_text_params) == -1) {
            GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
                ("Unable to draw text"), NULL);
            return GST_FLOW_ERROR;
          }
          text_cnt = 0;
        }
      }
    }

    for (cnt = 0; cnt < display_meta->num_lines; cnt++) {
      dsosdcoord->line_params[line_cnt++] = display_meta->line_params[cnt];
      if (line_cnt == MAX_OSD_ELEMS) {
        dsosdcoord->frame_line_params->num_lines = line_cnt;
        dsosdcoord->frame_line_params->line_params_list = dsosdcoord->line_params;
        dsosdcoord->frame_line_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoord->frame_line_params->mode = dsosdcoord->dsosdcoord_mode;
        if (nvll_osd_draw_lines (dsosdcoord->dsosdcoord_context,
                dsosdcoord->frame_line_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
              ("Unable to draw lines"), NULL);
          return GST_FLOW_ERROR;
        }
        line_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_arrows; cnt++) {
      dsosdcoord->arrow_params[arrow_cnt++] = display_meta->arrow_params[cnt];
      if (arrow_cnt == MAX_OSD_ELEMS) {
        dsosdcoord->frame_arrow_params->num_arrows = arrow_cnt;
        dsosdcoord->frame_arrow_params->arrow_params_list = dsosdcoord->arrow_params;
        dsosdcoord->frame_arrow_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoord->frame_arrow_params->mode = dsosdcoord->dsosdcoord_mode;
        if (nvll_osd_draw_arrows (dsosdcoord->dsosdcoord_context,
                dsosdcoord->frame_arrow_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
              ("Unable to draw arrows"), NULL);
          return GST_FLOW_ERROR;
        }
        arrow_cnt = 0;
      }
    }

    for (cnt = 0; cnt < display_meta->num_circles; cnt++) {
      dsosdcoord->circle_params[circle_cnt++] = display_meta->circle_params[cnt];
      if (circle_cnt == MAX_OSD_ELEMS) {
        dsosdcoord->frame_circle_params->num_circles = circle_cnt;
        dsosdcoord->frame_circle_params->circle_params_list =
            dsosdcoord->circle_params;
        dsosdcoord->frame_circle_params->buf_ptr = &surface->surfaceList[0];
        dsosdcoord->frame_circle_params->mode = dsosdcoord->dsosdcoord_mode;
        if (nvll_osd_draw_circles (dsosdcoord->dsosdcoord_context,
                dsosdcoord->frame_circle_params) == -1) {
          GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
              ("Unable to draw circles"), NULL);
          return GST_FLOW_ERROR;
        }
        circle_cnt = 0;
      }
    }
    i++;
  }

  dsosdcoord->num_rect = rect_cnt;
  dsosdcoord->num_segments = segment_cnt;
  dsosdcoord->num_strings = text_cnt;
  dsosdcoord->num_lines = line_cnt;
  dsosdcoord->num_arrows = arrow_cnt;
  dsosdcoord->num_circles = circle_cnt;
  if (rect_cnt != 0 && dsosdcoord->draw_bbox) {
    dsosdcoord->frame_rect_params->num_rects = dsosdcoord->num_rect;
    dsosdcoord->frame_rect_params->rect_params_list = dsosdcoord->rect_params;
    dsosdcoord->frame_rect_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_rect_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_draw_rectangles (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_rect_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
          ("Unable to draw rectangles"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (segment_cnt != 0 && dsosdcoord->draw_mask) {
    dsosdcoord->frame_mask_params->num_segments = dsosdcoord->num_segments;
    dsosdcoord->frame_mask_params->rect_params_list = dsosdcoord->mask_rect_params;
    dsosdcoord->frame_mask_params->mask_params_list = dsosdcoord->mask_params;
    dsosdcoord->frame_mask_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_mask_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_draw_segment_masks (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_mask_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
          ("Unable to draw segment masks"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if ((dsosdcoord->show_clock || text_cnt) && dsosdcoord->draw_text) {
    dsosdcoord->frame_text_params->num_strings = dsosdcoord->num_strings;
    dsosdcoord->frame_text_params->text_params_list = dsosdcoord->text_params;
    dsosdcoord->frame_text_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_text_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_put_text (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_text_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED, ("Unable to draw text"),
          NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (line_cnt != 0) {
    dsosdcoord->frame_line_params->num_lines = dsosdcoord->num_lines;
    dsosdcoord->frame_line_params->line_params_list = dsosdcoord->line_params;
    dsosdcoord->frame_line_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_line_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_draw_lines (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_line_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED, ("Unable to draw lines"),
          NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (arrow_cnt != 0) {
    dsosdcoord->frame_arrow_params->num_arrows = dsosdcoord->num_arrows;
    dsosdcoord->frame_arrow_params->arrow_params_list = dsosdcoord->arrow_params;
    dsosdcoord->frame_arrow_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_arrow_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_draw_arrows (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_arrow_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
          ("Unable to draw arrows"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  if (circle_cnt != 0) {
    dsosdcoord->frame_circle_params->num_circles = dsosdcoord->num_circles;
    dsosdcoord->frame_circle_params->circle_params_list = dsosdcoord->circle_params;
    dsosdcoord->frame_circle_params->buf_ptr = &surface->surfaceList[0];
    dsosdcoord->frame_circle_params->mode = dsosdcoord->dsosdcoord_mode;
    if (nvll_osd_draw_circles (dsosdcoord->dsosdcoord_context,
            dsosdcoord->frame_circle_params) == -1) {
      GST_ELEMENT_ERROR (dsosdcoord, RESOURCE, FAILED,
          ("Unable to draw circles"), NULL);
      return GST_FLOW_ERROR;
    }
  }

  nvtxRangePop ();
  dsosdcoord->frame_num++;

  nvds_set_output_system_timestamp (buf, GST_ELEMENT_NAME (dsosdcoord));

  gst_buffer_unmap (buf, &inmap);
  return GST_FLOW_OK;
}

/* Called when the plugin is destroyed.
 * Free all structures which have been malloc'd.
 */
static void
gst_ds_osdcoord_finalize (GObject * object)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (object);

  if (dsosdcoord->clock_text_params.font_params.font_name) {
    g_free ((char *) dsosdcoord->clock_text_params.font_params.font_name);
  }
  g_free (dsosdcoord->rect_params);
  g_free (dsosdcoord->mask_rect_params);
  g_free (dsosdcoord->mask_params);
  g_free (dsosdcoord->text_params);
  g_free (dsosdcoord->line_params);
  g_free (dsosdcoord->arrow_params);
  g_free (dsosdcoord->circle_params);

  g_free (dsosdcoord->frame_rect_params);
  g_free (dsosdcoord->frame_mask_params);
  g_free (dsosdcoord->frame_text_params);
  g_free (dsosdcoord->frame_line_params);
  g_free (dsosdcoord->frame_arrow_params);
  g_free (dsosdcoord->frame_circle_params);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void
gst_ds_osdcoord_class_init (GstDsOsdCoordClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_ds_osdcoord_transform_ip);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_ds_osdcoord_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_ds_osdcoord_stop);
  base_transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_ds_osdcoord_set_caps);

  gobject_class->set_property = gst_ds_osdcoord_set_property;
  gobject_class->get_property = gst_ds_osdcoord_get_property;
  gobject_class->finalize = gst_ds_osdcoord_finalize;

  base_transform_class->passthrough_on_same_caps = TRUE;

  g_object_class_install_property (gobject_class, PROP_SHOW_CLOCK,
      g_param_spec_boolean ("display-clock", "clock",
          "Whether to display clock", FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_TEXT,
      g_param_spec_boolean ("display-text", "text", "Whether to display text",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_BBOX,
      g_param_spec_boolean ("display-bbox", "text", "Whether to display bounding boxes",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_MASK,
      g_param_spec_boolean ("display-mask", "text", "Whether to display instance mask",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_COORD,
      g_param_spec_boolean ("display-coord", "text", "Whether to display coordinate",
	  TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT,
      g_param_spec_string ("clock-font", "clock-font",
          "Clock Font to be set",
          "DEFAULT_FONT",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_SIZE,
      g_param_spec_uint ("clock-font-size", "clock-font-size",
          "font size of the clock",
          0, MAX_FONT_SIZE, DEFAULT_FONT_SIZE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_X_OFFSET,
      g_param_spec_uint ("x-clock-offset", "x-clock-offset",
          "x-clock-offset",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_Y_OFFSET,
      g_param_spec_uint ("y-clock-offset", "y-clock-offset",
          "y-clock-offset",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CLOCK_COLOR,
      g_param_spec_uint ("clock-color", "clock-color",
          "clock-color",
          0, G_MAXUINT, G_MAXUINT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_PROCESS_MODE,
      g_param_spec_enum ("process-mode", "Process Mode",
          "Rect and text draw process mode",
          GST_TYPE_NV_OSD_PROCESS_MODE,
          GST_NV_OSD_DEFAULT_PROCESS_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_HW_BLEND_COLOR_ATTRS,
      g_param_spec_string ("hw-blend-color-attr", "HW Blend Color Attr",
          "color attributes for all classes,\n"
          "\t\t\t Use string with values of color class atrributes \n"
          "\t\t\t in ClassID (int), r(float), g(float), b(float), a(float)\n"
          "\t\t\t in order to set the property.\n"
          "\t\t\t Applicable only for HW mode on Jetson.\n"
          "\t\t\t e.g. 0,0.0,1.0,0.0,0.3:1,1.0,0.0,0.3,0.3",
          DEFAULT_CLR,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_GPU_DEVICE_ID,
      g_param_spec_uint ("gpu-id", "Set GPU Device ID",
          "Set GPU Device ID",
          0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  gst_element_class_set_details_simple (gstelement_class,
      "DsOsdCoord plugin",
      "DsOsdCoord functionality",
      "Gstreamer bounding box draw element",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&dsosdcoord_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&dsosdcoord_sink_factory));

  _dsmeta_quark = g_quark_from_static_string (NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void
gst_ds_osdcoord_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (object);

  switch (prop_id) {
    case PROP_SHOW_CLOCK:
      dsosdcoord->show_clock = g_value_get_boolean (value);
      break;
    case PROP_SHOW_TEXT:
      dsosdcoord->draw_text = g_value_get_boolean (value);
      break;
    case PROP_SHOW_BBOX:
      dsosdcoord->draw_bbox = g_value_get_boolean (value);
      break;
    case PROP_SHOW_MASK:
      dsosdcoord->draw_mask = g_value_get_boolean (value);
      break;
    case PROP_SHOW_COORD:
      dsosdcoord->display_coord = g_value_get_boolean (value);
      break;
    case PROP_CLOCK_FONT:
      if (dsosdcoord->clock_text_params.font_params.font_name) {
        g_free ((char *) dsosdcoord->clock_text_params.font_params.font_name);
      }
      dsosdcoord->clock_text_params.font_params.font_name =
          (gchar *) g_value_dup_string (value);
      break;
    case PROP_CLOCK_FONT_SIZE:
      dsosdcoord->clock_text_params.font_params.font_size =
          g_value_get_uint (value);
      break;
    case PROP_CLOCK_X_OFFSET:
      dsosdcoord->clock_text_params.x_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_Y_OFFSET:
      dsosdcoord->clock_text_params.y_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_COLOR:
      gst_ds_osdcoord_parse_color (dsosdcoord, g_value_get_uint (value));
      break;
    case PROP_PROCESS_MODE:
      dsosdcoord->dsosdcoord_mode = (NvOSD_Mode) g_value_get_enum (value);
      break;
    case PROP_HW_BLEND_COLOR_ATTRS:
      dsosdcoord->hw_blend = TRUE;
      gst_ds_osdcoord_parse_hw_blend_color_attrs (dsosdcoord,
          g_value_get_string (value));
      break;
    case PROP_GPU_DEVICE_ID:
      dsosdcoord->gpu_id = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void
gst_ds_osdcoord_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDsOsdCoord *dsosdcoord = GST_DSOSDCOORD (object);

  switch (prop_id) {
    case PROP_SHOW_CLOCK:
      g_value_set_boolean (value, dsosdcoord->show_clock);
      break;
    case PROP_SHOW_TEXT:
      g_value_set_boolean (value, dsosdcoord->draw_text);
      break;
    case PROP_SHOW_BBOX:
      g_value_set_boolean (value, dsosdcoord->draw_bbox);
      break;
    case PROP_SHOW_MASK:
      g_value_set_boolean (value, dsosdcoord->draw_mask);
      break;
    case PROP_SHOW_COORD:
      g_value_set_boolean (value, dsosdcoord->display_coord);
      break;
    case PROP_CLOCK_FONT:
      g_value_set_string (value, dsosdcoord->font);
      break;
    case PROP_CLOCK_FONT_SIZE:
      g_value_set_uint (value, dsosdcoord->clock_font_size);
      break;
    case PROP_CLOCK_X_OFFSET:
      g_value_set_uint (value, dsosdcoord->clock_text_params.x_offset);
      break;
    case PROP_CLOCK_Y_OFFSET:
      g_value_set_uint (value, dsosdcoord->clock_text_params.y_offset);
      break;
    case PROP_CLOCK_COLOR:
      g_value_set_uint (value, dsosdcoord->clock_color);
      break;
    case PROP_PROCESS_MODE:
      g_value_set_enum (value, dsosdcoord->dsosdcoord_mode);
      break;
    case PROP_HW_BLEND_COLOR_ATTRS:
      gst_ds_osdcoord_get_hw_blend_color_attrs (value, dsosdcoord);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint (value, dsosdcoord->gpu_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Set default values of certain properties.
 */
static void
gst_ds_osdcoord_init (GstDsOsdCoord * dsosdcoord)
{
  dsosdcoord->show_clock = FALSE;
  dsosdcoord->draw_text = TRUE;
  dsosdcoord->draw_bbox = TRUE;
  dsosdcoord->draw_mask = FALSE;
  dsosdcoord->display_coord = TRUE;
  dsosdcoord->clock_text_params.font_params.font_name = g_strdup (DEFAULT_FONT);
  dsosdcoord->clock_text_params.font_params.font_size = DEFAULT_FONT_SIZE;
  dsosdcoord->dsosdcoord_mode = GST_NV_OSD_DEFAULT_PROCESS_MODE;
  dsosdcoord->border_width = DEFAULT_BORDER_WIDTH;
  dsosdcoord->num_rect = 0;
  dsosdcoord->num_segments = 0;
  dsosdcoord->num_strings = 0;
  dsosdcoord->num_lines = 0;
  dsosdcoord->clock_text_params.font_params.font_color.red = 1.0;
  dsosdcoord->clock_text_params.font_params.font_color.green = 0.0;
  dsosdcoord->clock_text_params.font_params.font_color.blue = 0.0;
  dsosdcoord->clock_text_params.font_params.font_color.alpha = 1.0;
  dsosdcoord->rect_params = g_new0 (NvOSD_RectParams, MAX_OSD_ELEMS);
  dsosdcoord->mask_rect_params = g_new0 (NvOSD_RectParams, MAX_OSD_ELEMS);
  dsosdcoord->mask_params = g_new0 (NvOSD_MaskParams, MAX_OSD_ELEMS);
  dsosdcoord->text_params = g_new0 (NvOSD_TextParams, MAX_OSD_ELEMS);
  dsosdcoord->line_params = g_new0 (NvOSD_LineParams, MAX_OSD_ELEMS);
  dsosdcoord->arrow_params = g_new0 (NvOSD_ArrowParams, MAX_OSD_ELEMS);
  dsosdcoord->circle_params = g_new0 (NvOSD_CircleParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_rect_params = g_new0 (NvOSD_FrameRectParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_mask_params = g_new0 (NvOSD_FrameSegmentMaskParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_text_params = g_new0 (NvOSD_FrameTextParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_line_params = g_new0 (NvOSD_FrameLineParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_arrow_params = g_new0 (NvOSD_FrameArrowParams, MAX_OSD_ELEMS);
  dsosdcoord->frame_circle_params =
      g_new0 (NvOSD_FrameCircleParams, MAX_OSD_ELEMS);
  dsosdcoord->hw_blend = FALSE;
}

/**
 * Set color of text for clock, if enabled.
 */
static gboolean
gst_ds_osdcoord_parse_color (GstDsOsdCoord * dsosdcoord, guint clock_color)
{
  dsosdcoord->clock_text_params.font_params.font_color.red =
      (gfloat) ((clock_color & 0xff000000) >> 24) / 255;
  dsosdcoord->clock_text_params.font_params.font_color.green =
      (gfloat) ((clock_color & 0x00ff0000) >> 16) / 255;
  dsosdcoord->clock_text_params.font_params.font_color.blue =
      (gfloat) ((clock_color & 0x0000ff00) >> 8) / 255;
  dsosdcoord->clock_text_params.font_params.font_color.alpha =
      (gfloat) ((clock_color & 0x000000ff)) / 255;
  return TRUE;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean
dsosdcoord_init (GstPlugin * dsosdcoord)
{
  GST_DEBUG_CATEGORY_INIT (gst_ds_osdcoord_debug, "dsosdcoord", 0, "dsosdcoord plugin");

  return gst_element_register (dsosdcoord, "dsosdcoord", GST_RANK_PRIMARY,
      GST_TYPE_DSOSDCOORD);
}

static gboolean
gst_ds_osdcoord_parse_hw_blend_color_attrs (GstDsOsdCoord * dsosdcoord,
    const gchar * arr)
{
  gchar *str = (gchar *) arr;
  int idx = 0;
  int class_id = 0;

  while (str != NULL && str[0] != '\0') {
    class_id = atoi (str);
    if (class_id >= MAX_BG_CLR) {
      g_print ("dsosdcoord: class_id %d is exceeding than %d\n", class_id,
          MAX_BG_CLR);
      exit (-1);
    }
    dsosdcoord->color_info[idx].id = class_id;
    str = g_strstr_len (str, -1, ",") + 1;

    dsosdcoord->color_info[idx].color.red = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoord->color_info[idx].color.green = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoord->color_info[idx].color.blue = atof (str);
    str = g_strstr_len (str, -1, ",") + 1;
    dsosdcoord->color_info[idx].color.alpha = atof (str);
    str = g_strstr_len (str, -1, ":");

    if (str) {
      str = str + 1;
    }
    idx++;
    if (idx >= MAX_BG_CLR) {
      g_print ("idx (%d) entries exceeded MAX_CLASSES %d\n", idx, MAX_BG_CLR);
      break;
    }
  }

  dsosdcoord->num_class_entries = idx;
  return TRUE;
}

static gboolean
gst_ds_osdcoord_get_hw_blend_color_attrs (GValue * value, GstDsOsdCoord * dsosdcoord)
{
  int idx = 0;
  gchar arr[100];

  while (idx < (dsosdcoord->num_class_entries - 1)) {
    sprintf (arr, "%d,%f,%f,%f,%f:",
        dsosdcoord->color_info[idx].id, dsosdcoord->color_info[idx].color.red,
        dsosdcoord->color_info[idx].color.green,
        dsosdcoord->color_info[idx].color.blue,
        dsosdcoord->color_info[idx].color.alpha);
    idx++;
  }
  sprintf (arr, "%d,%f,%f,%f,%f:",
      dsosdcoord->color_info[idx].id, dsosdcoord->color_info[idx].color.red,
      dsosdcoord->color_info[idx].color.green,
      dsosdcoord->color_info[idx].color.blue,
      dsosdcoord->color_info[idx].color.alpha);

  g_value_set_string (value, arr);
  return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "dsosdcoord"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_dsosdcoord,
    PACKAGE_DESCRIPTION,
    dsosdcoord_init, DS_VERSION, PACKAGE_LICENSE, PACKAGE_NAME, PACKAGE_URL)

