# extract-metadata-from-ai-model-on-deepstream
extract-metadata-from-ai-model-on-deepstream-with-gstreamer は、GStreamer を使用して、DeepStream上でAIモデルからメタデータを抽出するマイクロサービスです。
本マイクロサービスでは、以下のメタデータを取得できます。

- フレーム番号
- ラベル
- バウンディングボックスの座標

## 動作環境
- NVIDIA Jetson
    - Jetpack 4.6
    - DeepStream 6.0
- GStreamer 1.14
- GNU Make

## 前提条件
本マイクロサービスを使用するにあたり、事前に[GStreamer](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.01.html#)をエッジデバイス上にインストールしてください。


## 動作手順
### dsosdcoordのビルド
バウンディングボックスやラベルをディスプレイに表示し、それらのメタデータをコンソールに表示するプラグインを以下のコマンドでビルドします。
```sh
make build
```

### ストリーミングの開始
以下のコマンドでストリーミングを開始します。
```sh
make start
```

## 本レポジトリにおけるGStreamerの修正部分について
本レポジトリでは、基本的に[GStreamer](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.01.html#)のリソースをそのまま活用していますが、GStreamerのリソースのうち、[Gst-nvdsosd](https://docs.nvidia.com/metropolis/deepstream/5.0DP/plugin-manual/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.3.06.html#wwconnect_header)のリソースのみ、バウンディングボックスの座標等の設定パラメータを追加するため、変更を加えています。  
設定パラメータを追加した箇所は、gst-dsosdcoord / gstdsosdcoord.c のファイルにおける、以下の部分です。

```
if (dsosdcoord->display_coord) {
      COORD top_left, bottom_right;
      top_left.x = object_meta->rect_params.left;
      top_left.y = object_meta->rect_params.top;
      bottom_right.x = object_meta->rect_params.left + object_meta->rect_params.width;
      bottom_right.y = object_meta->rect_params.top + object_meta->rect_params.height;
      g_print("%u: %s, ", dsosdcoord->frame_num, object_meta->text_params.display_text);
      g_print ("Top Left: (%f, %f), Bottom Right: (%f, %f)\n", top_left.x, top_left.y, bottom_right.x, bottom_right.y);
    }
```





