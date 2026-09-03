#pragma once

#include <Arduino.h>

#define GALLERY_DIR "/gallery"
#define GALLERY_THUMB_DIR "/gallery/thumbnails"
#define GALLERY_THUMB_W 160
#define GALLERY_THUMB_H 112 // multiple of 16 for JPEG MCU
#define GALLERY_MAX_FILES 200
#define GALLERY_NAME_LEN 32

bool galleryEnsureDir();
// Move temp download (IMAGE_FS_PATH) into /gallery/NNNNN.jpg|.png.
bool gallerySaveFromTemp(const char *tempPath, bool isJpeg, char *outPath,
                         size_t outLen);
bool galleryDelete(const char *path);
bool galleryThumbPathFor(const char *imagePath, char *outPath, size_t outLen);
bool galleryThumbPathAt(int index, char *outPath, size_t outLen);
bool galleryMakeThumb(const char *imagePath);
int galleryCount();
// index 0 = oldest.
bool galleryPathAt(int index, char *outPath, size_t outLen);
int galleryLatestIndex();
int galleryClearAll();
