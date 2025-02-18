#include "image.h"

#if defined(FF_HAVE_IMAGEMAGICK7) || defined(FF_HAVE_IMAGEMAGICK6)

#define FF_KITTY_MAX_CHUNK_SIZE 4096

#define FF_CACHE_FILE_HEIGHT "height"
#define FF_CACHE_FILE_SIXEL "sixel"
#define FF_CACHE_FILE_KITTY_COMPRESSED "kittyc"
#define FF_CACHE_FILE_KITTY_UNCOMPRESSED "kittyu"
#define FF_CACHE_FILE_CHAFA "chafa"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef FF_HAVE_ZLIB
#include <zlib.h>

static bool compressBlob(const FFinstance* instance, void** blob, size_t* length)
{
    FF_LIBRARY_LOAD(zlib, instance->config.libZ, false, "libz.so", 2)
    FF_LIBRARY_LOAD_SYMBOL(zlib, compressBound, false)
    FF_LIBRARY_LOAD_SYMBOL(zlib, compress2, false)

    uLong compressedLength = ffcompressBound(*length);
    void* compressed = malloc(compressedLength);
    if(compressed == NULL)
    {
        dlclose(zlib);
        return false;
    }

    int compressResult = ffcompress2(compressed, &compressedLength, *blob, *length, Z_BEST_COMPRESSION);
    dlclose(zlib);
    if(compressResult != Z_OK)
    {
        free(compressed);
        return false;
    }

    free(*blob);

    *length = (size_t) compressedLength;
    *blob = compressed;
    return true;
}

#endif // FF_HAVE_ZLIB

//We use only the defines from here, that are exactly the same in both versions
#ifdef FF_HAVE_IMAGEMAGICK7
    #include <MagickCore/MagickCore.h>
#else
    #include <magick/MagickCore.h>
#endif

typedef struct ImageData
{
    FF_LIBRARY_SYMBOL(CopyMagickString)
    FF_LIBRARY_SYMBOL(ImageToBlob)
    FF_LIBRARY_SYMBOL(Base64Encode)

    ImageInfo* imageInfo;
    Image* image;
    ExceptionInfo* exceptionInfo;
} ImageData;

static inline bool checkAllocationResult(void* data, size_t length)
{
    if(data == NULL)
        return false;

    if(length == 0)
    {
        free(data);
        return false;
    }

    return true;
}

static void finishPrinting(FFinstance* instance, const FFLogoRequestData* requestData)
{
    instance->state.logoWidth = (uint32_t) (requestData->imagePixelWidth / requestData->characterPixelWidth) + instance->config.logoPaddingLeft + instance->config.logoPaddingRight;

    if(instance->config.logoHeight == 0)
        instance->state.logoHeight = (uint32_t) (requestData->imagePixelHeight / requestData->characterPixelHeight);
    else
        instance->state.logoHeight = instance->config.logoHeight;

    //Seems to be required on all consoles that support sixel. TBH i don't know why.
    if(requestData->type == FF_LOGO_TYPE_SIXEL)
        instance->state.logoHeight += 1;

    //Chafa ascii images don't have a newline at the end
    if(requestData->type == FF_LOGO_TYPE_CHAFA)
        instance->state.logoHeight -= 1;

    fputs("\033[9999999D", stdout);
    printf("\033[%uA", instance->state.logoHeight);
}

static void writeResult(FFinstance* instance, FFLogoRequestData* requestData, const FFstrbuf* result, const char* cacheFileName)
{
    //Write result to stdout
    ffPrintCharTimes(' ', instance->config.logoPaddingLeft);
    fflush(stdout);
    ffWriteFDContent(STDOUT_FILENO, result);

    //Write result to cache file
    uint32_t cacheDirLength = requestData->cacheDir.length;
    ffStrbufAppendS(&requestData->cacheDir, cacheFileName);
    ffWriteFileContent(requestData->cacheDir.chars, result);
    ffStrbufSubstrBefore(&requestData->cacheDir, cacheDirLength);

    //If no custom height is given, we need to save the height in "height" file
    if(instance->config.logoHeight == 0)
    {
        ffStrbufAppendS(&requestData->cacheDir, FF_CACHE_FILE_HEIGHT);
        FFstrbuf content;
        content.chars = (char*) &requestData->imagePixelHeight;
        content.length = sizeof(requestData->imagePixelHeight);
        ffWriteFileContent(requestData->cacheDir.chars, &content);
        ffStrbufSubstrBefore(&requestData->cacheDir, cacheDirLength);
    }

    //Write escape codes to stdout
    finishPrinting(instance, requestData);
}

static bool printImageSixel(FFinstance* instance, FFLogoRequestData* requestData, const ImageData* imageData)
{
    imageData->ffCopyMagickString(imageData->imageInfo->magick, "SIXEL", 6);

    size_t length;
    void* blob = imageData->ffImageToBlob(imageData->imageInfo, imageData->image, &length, imageData->exceptionInfo);
    if(!checkAllocationResult(blob, length))
        return false;

    FFstrbuf result;
    result.chars = (char*) blob;
    result.length = (uint32_t) length;

    writeResult(instance, requestData, &result, FF_CACHE_FILE_SIXEL);

    free(blob);
    return true;
}

static void appendKittyChunk(FFstrbuf* result, const char** blob, size_t* length, bool printEscapeCode)
{
    uint32_t chunkSize = *length > FF_KITTY_MAX_CHUNK_SIZE ? FF_KITTY_MAX_CHUNK_SIZE : (uint32_t) *length;

    if(printEscapeCode)
        ffStrbufAppendS(result, "\033_G");
    else
        ffStrbufAppendC(result, ',');

    ffStrbufAppendS(result, chunkSize != *length ? "m=1" : "m=0");
    ffStrbufAppendC(result, ';');
    ffStrbufAppendNS(result, chunkSize, *blob);
    ffStrbufAppendS(result, "\033\\");
    *length -= chunkSize;
    *blob += chunkSize;
}

static bool printImageKitty(FFinstance* instance, FFLogoRequestData* requestData, const ImageData* imageData)
{
    imageData->ffCopyMagickString(imageData->imageInfo->magick, "RGBA", 5);

    size_t length;
    void* blob = imageData->ffImageToBlob(imageData->imageInfo, imageData->image, &length, imageData->exceptionInfo);
    if(!checkAllocationResult(blob, length))
        return false;

    #ifdef FF_HAVE_ZLIB
        bool isCompressed = compressBlob(instance, &blob, &length);
    #else
        bool isCompressed = false;
    #endif

    char* chars = imageData->ffBase64Encode(blob, length, &length);
    free(blob);
    if(!checkAllocationResult(chars, length))
        return false;

    FFstrbuf result;
    ffStrbufInitA(&result, (uint32_t) (length + 1024));

    const char* currentPos = chars;
    size_t remainingLength = length;

    ffStrbufAppendF(&result, "\033_Ga=T,f=32,s=%u,v=%u", requestData->imagePixelWidth, requestData->imagePixelHeight);
    if(isCompressed)
        ffStrbufAppendS(&result, ",o=z");
    appendKittyChunk(&result, &currentPos, &remainingLength, false);
    while(remainingLength > 0)
        appendKittyChunk(&result, &currentPos, &remainingLength, true);

    writeResult(instance, requestData, &result, isCompressed ? FF_CACHE_FILE_KITTY_COMPRESSED : FF_CACHE_FILE_KITTY_UNCOMPRESSED);

    ffStrbufDestroy(&result);
    free(chars);
    return true;
}

#ifdef FF_HAVE_CHAFA
#include <chafa/chafa.h>
static bool printImageChafa(FFinstance* instance, FFLogoRequestData* requestData, const ImageData* imageData)
{
    FF_LIBRARY_LOAD(chafa, instance->config.libChafa, false, "libchafa.so", 1)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_symbol_map_new, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_symbol_map_add_by_tags, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_config_new, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_config_set_geometry, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_config_set_symbol_map, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_new, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_draw_all_pixels, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_print, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_unref, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_canvas_config_unref, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, chafa_symbol_map_unref, false)
    FF_LIBRARY_LOAD_SYMBOL(chafa, g_string_free, false)

    imageData->ffCopyMagickString(imageData->imageInfo->magick, "RGBA", 5);
    size_t length;
    void* blob = imageData->ffImageToBlob(imageData->imageInfo, imageData->image, &length, imageData->exceptionInfo);
    if(!checkAllocationResult(blob, length))
        return false;

    ChafaSymbolMap* symbolMap = ffchafa_symbol_map_new();
    ffchafa_symbol_map_add_by_tags(symbolMap, CHAFA_SYMBOL_TAG_ALL);

    //imagePixelHeight is calculated, as if the image was already resized
    gint logoCharacterHeight = (gint) (requestData->imagePixelHeight / requestData->characterPixelHeight);

    ChafaCanvasConfig* canvasConfig = ffchafa_canvas_config_new();
    ffchafa_canvas_config_set_geometry(canvasConfig, (gint) instance->config.logoWidth, logoCharacterHeight);
    ffchafa_canvas_config_set_symbol_map(canvasConfig, symbolMap);

    ChafaCanvas* canvas = ffchafa_canvas_new(canvasConfig);
    ffchafa_canvas_draw_all_pixels(
        canvas,
        CHAFA_PIXEL_RGBA8_UNASSOCIATED,
        blob,
        (gint) imageData->image->columns,
        (gint) imageData->image->rows,
        (gint) imageData->image->columns * 4
    );

    GString* str = ffchafa_canvas_print(canvas, NULL);

    FFstrbuf result;
    result.chars = str->str;
    result.length = (uint32_t) str->len;
    writeResult(instance, requestData, &result, FF_CACHE_FILE_CHAFA);

    ffg_string_free(str, TRUE);
    ffchafa_canvas_unref(canvas);
    ffchafa_canvas_config_unref(canvasConfig);
    ffchafa_symbol_map_unref(symbolMap);

    return true;
}
#endif

FFLogoImageResult ffLogoPrintImageImpl(FFinstance* instance, FFLogoRequestData* requestData, const FFIMData* imData)
{
    FF_LIBRARY_LOAD_SYMBOL(imData->library, AcquireExceptionInfo, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL(imData->library, DestroyExceptionInfo, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL(imData->library, AcquireImageInfo, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL(imData->library, DestroyImageInfo, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL(imData->library, ReadImage, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL(imData->library, DestroyImage, FF_LOGO_IMAGE_RESULT_INIT_ERROR)

    ImageData imageData;

    FF_LIBRARY_LOAD_SYMBOL_ADRESS(imData->library, imageData.ffCopyMagickString, CopyMagickString, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL_ADRESS(imData->library, imageData.ffImageToBlob, ImageToBlob, FF_LOGO_IMAGE_RESULT_INIT_ERROR)
    FF_LIBRARY_LOAD_SYMBOL_ADRESS(imData->library, imageData.ffBase64Encode, Base64Encode, FF_LOGO_IMAGE_RESULT_INIT_ERROR)

    imageData.exceptionInfo = ffAcquireExceptionInfo();
    if(imageData.exceptionInfo == NULL)
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;

    ImageInfo* imageInfoIn = ffAcquireImageInfo();
    if(imageInfoIn == NULL)
    {
        ffDestroyExceptionInfo(imageData.exceptionInfo);
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
    }

    //+1, because we need to copy the null byte too
    imageData.ffCopyMagickString(imageInfoIn->filename, instance->config.logoSource.chars, instance->config.logoSource.length + 1);

    Image* originalImage = ffReadImage(imageInfoIn, imageData.exceptionInfo);
    ffDestroyImageInfo(imageInfoIn);
    if(originalImage == NULL)
    {
        ffDestroyExceptionInfo(imageData.exceptionInfo);
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
    }

    //imagePixelWidth is always set in ffLogoPrintImageIfExists
    //imagePixelHeigght is set, if a custom one is given. Otherwise calculate right aspect ratio here
    if(requestData->imagePixelHeight == 0)
    {
        requestData->imagePixelHeight = (uint32_t) ((requestData->imagePixelWidth / (double) originalImage->columns) * (double) originalImage->rows);
        if(requestData->imagePixelHeight == 0)
        {
            ffDestroyImage(originalImage);
            ffDestroyExceptionInfo(imageData.exceptionInfo);
            return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
        }
    }

    if(instance->config.logoType != FF_LOGO_TYPE_CHAFA)
    {
        imageData.image = (Image*) imData->resizeFunc(originalImage, requestData->imagePixelWidth, requestData->imagePixelHeight, imageData.exceptionInfo);
        ffDestroyImage(originalImage);
        if(imageData.image == NULL)
        {
            ffDestroyExceptionInfo(imageData.exceptionInfo);
            return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
        }
    }
    else
    {
        //We don't want to resize the image for chafa, because chafa does that while converting to ASCII.
        imageData.image = originalImage;
    }

    imageData.imageInfo = ffAcquireImageInfo();
    if(imageData.imageInfo == NULL)
    {
        ffDestroyImage(imageData.image);
        ffDestroyExceptionInfo(imageData.exceptionInfo);
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
    }

    bool printSuccessful;
    if(requestData->type == FF_LOGO_TYPE_SIXEL)
        printSuccessful = printImageSixel(instance, requestData, &imageData);
    else if(requestData->type == FF_LOGO_TYPE_KITTY)
        printSuccessful = printImageKitty(instance, requestData, &imageData);
    else if(requestData->type == FF_LOGO_TYPE_CHAFA)
        #ifdef FF_HAVE_CHAFA
            printSuccessful = printImageChafa(instance, requestData, &imageData);
        #else //should never happen at this point
            printSuccessful = false;
        #endif
    else //should never happen at this point
        printSuccessful = false;

    ffDestroyImageInfo(imageData.imageInfo);
    ffDestroyImage(imageData.image);
    ffDestroyExceptionInfo(imageData.exceptionInfo);

    return printSuccessful ? FF_LOGO_IMAGE_RESULT_SUCCESS : FF_LOGO_IMAGE_RESULT_RUN_ERROR;
}

static int getCacheFD(FFLogoRequestData* requestData, const char* fileName)
{
    uint32_t cacheDirLength = requestData->cacheDir.length;
    ffStrbufAppendS(&requestData->cacheDir, fileName);
    int fd = open(requestData->cacheDir.chars, O_RDONLY);
    ffStrbufSubstrBefore(&requestData->cacheDir, cacheDirLength);
    return fd;
}

static bool printCached(FFinstance* instance, FFLogoRequestData* requestData)
{
    //If no custom height is given, read the height from file
    if(requestData->imagePixelHeight == 0)
    {
        uint32_t cacheDirLength = requestData->cacheDir.length;
        ffStrbufAppendS(&requestData->cacheDir, FF_CACHE_FILE_HEIGHT);

        FFstrbuf content;
        ffStrbufInitA(&content, sizeof(requestData->imagePixelHeight) + 1); // +1 because strbuf is null terminated
        memset(content.chars, 0, sizeof(requestData->imagePixelHeight));

        ffAppendFileContent(requestData->cacheDir.chars, &content);
        memcpy(&requestData->imagePixelHeight, content.chars, sizeof(requestData->imagePixelHeight));

        ffStrbufDestroy(&content);
        ffStrbufSubstrBefore(&requestData->cacheDir, cacheDirLength);

        if(requestData->imagePixelHeight == 0)
            return false;
    }

    int fd;
    if(requestData->type == FF_LOGO_TYPE_KITTY)
    {
        fd = getCacheFD(requestData, FF_CACHE_FILE_KITTY_COMPRESSED);
        if(fd == -1)
            fd = getCacheFD(requestData, FF_CACHE_FILE_KITTY_UNCOMPRESSED);
    }
    else if(requestData->type == FF_LOGO_TYPE_SIXEL)
        fd = getCacheFD(requestData, FF_CACHE_FILE_SIXEL);
    else if(requestData->type == FF_LOGO_TYPE_CHAFA)
        fd = getCacheFD(requestData, FF_CACHE_FILE_CHAFA);
    else
        fd = -1; //should never happen at this point

    if(fd == -1)
        return false;

    ffPrintCharTimes(' ', instance->config.logoPaddingLeft);
    fflush(stdout);

    char buffer[32768];
    ssize_t readBytes;
    while((readBytes = read(fd, buffer, sizeof(buffer))) > 0)
        (void) write(STDOUT_FILENO, buffer, (size_t) readBytes);

    close(fd);

    finishPrinting(instance, requestData);
    return true;
}

static bool getTermInfo(struct winsize* winsize)
{
    //Initialize every member to 0, because it isn't guaranteed that every terminal sets them all
    memset(winsize, 0, sizeof(struct winsize));

    ioctl(STDOUT_FILENO, TIOCGWINSZ, winsize);

    if(winsize->ws_row == 0 || winsize->ws_col == 0)
        ffGetTerminalResponse("\033[18t", "\033[8;%hu;%hut", &winsize->ws_row, &winsize->ws_col);

    if(winsize->ws_ypixel == 0 || winsize->ws_xpixel == 0)
        ffGetTerminalResponse("\033[14t", "\033[4;%hu;%hut", &winsize->ws_ypixel, &winsize->ws_xpixel);

    return
        winsize->ws_row > 0 &&
        winsize->ws_col > 0 &&
        winsize->ws_ypixel > 0 &&
        winsize->ws_xpixel > 0;
}

FFLogoImageResult ffLogoPrintImageIfExists(FFinstance* instance, FFLogoType type)
{
    #ifndef FF_HAVE_CHAFA
        if(type == FF_LOGO_TYPE_CHAFA)
            return FF_LOGO_IMAGE_RESULT_INIT_ERROR;
    #endif

    FFLogoRequestData requestData;

    if(!getTermInfo(&requestData.winsize))
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;

    requestData.type = type;
    requestData.characterPixelWidth = requestData.winsize.ws_xpixel / (double) requestData.winsize.ws_col;
    requestData.characterPixelHeight = requestData.winsize.ws_ypixel / (double) requestData.winsize.ws_row;
    requestData.imagePixelWidth = (uint32_t) (instance->config.logoWidth * requestData.characterPixelWidth);
    if(requestData.imagePixelWidth < 1)
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;

    if(instance->config.logoHeight > 0)
    {
        requestData.imagePixelHeight = (uint32_t) (instance->config.logoHeight * requestData.characterPixelHeight);
        if(requestData.imagePixelHeight == 0)
            return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
    }
    else
        requestData.imagePixelHeight = 0;

    ffStrbufInitA(&requestData.cacheDir, PATH_MAX * 2);
    ffStrbufAppend(&requestData.cacheDir, &instance->state.cacheDir);
    ffStrbufAppendS(&requestData.cacheDir, "images");

    ffStrbufEnsureFree(&requestData.cacheDir, PATH_MAX);
    if(realpath(instance->config.logoSource.chars, requestData.cacheDir.chars + requestData.cacheDir.length) == NULL)
    {
        //We can safely return here, because if realpath failed, we surely won't be able to read the file
        ffStrbufDestroy(&requestData.cacheDir);
        return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
    }
    ffStrbufRecalculateLength(&requestData.cacheDir);
    if(!ffStrbufEndsWithC(&requestData.cacheDir, '/'))
        ffStrbufAppendC(&requestData.cacheDir, '/');

    ffStrbufAppendF(&requestData.cacheDir, "%u*%u", requestData.imagePixelWidth, requestData.imagePixelHeight);
    ffStrbufAppendC(&requestData.cacheDir, '/');

    if(!instance->config.recache && printCached(instance, &requestData))
    {
        ffStrbufDestroy(&requestData.cacheDir);
        return FF_LOGO_IMAGE_RESULT_SUCCESS;
    }

    FFLogoImageResult result = FF_LOGO_IMAGE_RESULT_INIT_ERROR;

    #ifdef FF_HAVE_IMAGEMAGICK7
        result = ffLogoPrintImageIM7(instance, &requestData);
    #endif

    #ifdef FF_HAVE_IMAGEMAGICK6
        if(result == FF_LOGO_IMAGE_RESULT_INIT_ERROR)
            result = ffLogoPrintImageIM6(instance, &requestData);
    #endif

    ffStrbufDestroy(&requestData.cacheDir);
    return result;
}

#else //FF_HAVE_IMAGEMAGICK{6, 7}
FFLogoImageResult ffLogoPrintImageIfExists(FFinstance* instance, FFLogoType type)
{
    FF_UNUSED(instance, type);
    return FF_LOGO_IMAGE_RESULT_RUN_ERROR;
}
#endif //FF_HAVE_IMAGEMAGICK{6, 7}
