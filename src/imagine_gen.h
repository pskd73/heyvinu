#pragma once

#include <stddef.h>

/** OpenRouter image gen (or edit-by-reference). Saves via gallerySaveFromTemp. */
bool imagineGenerate(const char *prompt, char *outPath, size_t outLen,
                     const char *referencePath = nullptr);
