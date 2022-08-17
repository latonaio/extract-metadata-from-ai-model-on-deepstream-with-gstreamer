# Self-Documented Makefile
# https://marmelab.com/blog/2016/02/29/auto-documented-makefile.html
.PHONY: help
help:
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

NVDS_VERSION:=6.0

GST_PLUGIN_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/gst-plugins
OSDCOORD_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/gst-plugins/gst-dsosdcoord

build: ## dsosdcoordのビルド
	sudo cp -r gst-dsosdcoord $(GST_PLUGIN_DIR)
	sudo make -C $(OSDCOORD_DIR)
	sudo make -C $(OSDCOORD_DIR) install
