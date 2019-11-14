/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "SettingsStructs.h"
#include "GlobalPrefs.h"
#include "ParseCommandLine.h"

#include "TreeModel.h"
#include "BaseEngine.h"
#include "EngineManager.h"

// TODO(port)
// extern "C" void fz_redirect_dll_io_to_console();

void TestRenderPage(const CommandLineInfo& i) {
    if (i.showConsole) {
        RedirectIOToConsole();
        // fz_redirect_dll_io_to_console();
    }

    if (i.pageNumber == -1) {
        printf("pageNumber is -1\n");
        return;
    }
    auto files = i.fileNames;
    if (files.size() == 0) {
        printf("no file provided\n");
        return;
    }
    float zoom = ZOOM_ACTUAL_SIZE;
    if (i.startZoom != INVALID_ZOOM) {
        zoom = i.startZoom;
    }
    for (auto fileName : files) {
        OwnedData fileNameUtf(str::conv::ToUtf8(fileName));
        printf("rendering page %d for '%s', zoom: %.2f\n", i.pageNumber, fileNameUtf.Get(), zoom);
        auto engine = EngineManager::CreateEngine(fileName);
        if (engine == nullptr) {
            printf("failed to create engine\n");
            continue;
        }
        auto bmp = engine->RenderBitmap(i.pageNumber, zoom, 0);
        if (bmp == nullptr) {
            printf("failed to render page\n");
        }
        delete bmp;
        delete engine;
    }
}

static void extractPageText(BaseEngine* engine, int pageNo) {
    RectI* coordsOut; // not using the result, only to trigger the code path
    WCHAR* uni = engine->ExtractPageText(pageNo, L"_", &coordsOut);
    OwnedData utf = str::conv::ToUtf8(uni);
    printf("text on page %d: '", pageNo);
    // print characters as hex because I don't know what kind of locale-specific mangling
    // printf() might do
    int idx = 0;
    while (utf.Get()[idx] != 0) {
        char c = utf.Get()[idx++];
        printf("%02x ", (unsigned char)c);
    }
    printf("'\n");
    free(uni);
    free(coordsOut);
}

void TestExtractPage(const CommandLineInfo& ci) {
    if (ci.showConsole) {
        RedirectIOToConsole();
        // fz_redirect_dll_io_to_console();
    }

    int pageNo = ci.pageNumber;

    auto files = ci.fileNames;
    if (files.size() == 0) {
        printf("no file provided\n");
        return;
    }
    for (auto fileName : files) {
        OwnedData fileNameUtf(str::conv::ToUtf8(fileName));
        auto engine = EngineManager::CreateEngine(fileName);
        if (engine == nullptr) {
            printf("failed to create engine for file '%s'\n", fileNameUtf.Get());
            continue;
        }
        if (pageNo < 0) {
            int nPages = engine->PageCount();
            for (int i = 1; i <= nPages; i++) {
                extractPageText(engine, i);
            }
        } else {
            extractPageText(engine, pageNo);
        }

        delete engine;
    }
}
