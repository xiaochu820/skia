/*
 * Copyright 2018 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if SK_SUPPORT_GPU
#include "GrBackendSurface.h"
#include "GrContext.h"
#include "GrGLInterface.h"
#include "GrGLTypes.h"
#endif

#include "SkBlendMode.h"
#include "SkBlurTypes.h"
#include "SkCanvas.h"
#include "SkColor.h"
#include "SkCornerPathEffect.h"
#include "SkDashPathEffect.h"
#include "SkData.h"
#include "SkDiscretePathEffect.h"
#include "SkEncodedImageFormat.h"
#include "SkFilterQuality.h"
#include "SkFont.h"
#include "SkFontMgr.h"
#include "SkFontMgrPriv.h"
#include "SkFontTypes.h"
#include "SkGradientShader.h"
#include "SkImage.h"
#include "SkImageInfo.h"
#include "SkImageShader.h"
#include "SkMakeUnique.h"
#include "SkMaskFilter.h"
#include "SkPaint.h"
#include "SkParsePath.h"
#include "SkPath.h"
#include "SkPathEffect.h"
#include "SkPathOps.h"
#include "SkScalar.h"
#include "SkShader.h"
#include "SkShadowUtils.h"
#include "SkString.h"
#include "SkStrokeRec.h"
#include "SkSurface.h"
#include "SkSurfaceProps.h"
#include "SkTrimPathEffect.h"
#include "SkTypeface.h"
#include "SkTypes.h"
#include "SkVertices.h"

#include <iostream>
#include <string>

#include "WasmAliases.h"
#include <emscripten.h>
#include <emscripten/bind.h>

#if SK_SUPPORT_GPU
#include <GL/gl.h>
#include <emscripten/html5.h>
#endif

// Aliases for less typing
using BoneIndices = SkVertices::BoneIndices;
using BoneWeights = SkVertices::BoneWeights;
using Bone        = SkVertices::Bone;

#if SK_SUPPORT_GPU
// Wraps the WebGL context in an SkSurface and returns it.
// This function based on the work of
// https://github.com/Zubnix/skia-wasm-port/, used under the terms of the MIT license.
sk_sp<SkSurface> getWebGLSurface(EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context, int width, int height) {
    EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(context);
    if (r < 0) {
        printf("failed to make webgl context current %d\n", r);
        return nullptr;
    }

    glClearColor(0, 0, 0, 0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // setup GrContext
    auto interface = GrGLMakeNativeInterface();

    // setup contexts
    sk_sp<GrContext> grContext(GrContext::MakeGL(interface));

    // Wrap the frame buffer object attached to the screen in a Skia render target so Skia can
    // render to it
    GrGLint buffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &buffer);
    GrGLFramebufferInfo info;
    info.fFBOID = (GrGLuint) buffer;
    SkColorType colorType;

    info.fFormat = GL_RGBA8;
    colorType = kRGBA_8888_SkColorType;

    GrBackendRenderTarget target(width, height, 0, 8, info);

    sk_sp<SkSurface> surface(SkSurface::MakeFromBackendRenderTarget(grContext.get(), target,
                                                                    kBottomLeft_GrSurfaceOrigin,
                                                                    colorType, nullptr, nullptr));
    return surface;
}
#endif

struct SimpleMatrix {
    SkScalar scaleX, skewX,  transX;
    SkScalar skewY,  scaleY, transY;
    SkScalar pers0,  pers1,  pers2;
};

SkMatrix toSkMatrix(const SimpleMatrix& sm) {
    return SkMatrix::MakeAll(sm.scaleX, sm.skewX , sm.transX,
                             sm.skewY , sm.scaleY, sm.transY,
                             sm.pers0 , sm.pers1 , sm.pers2);
}

SimpleMatrix toSimpleSkMatrix(const SkMatrix& sm) {
    SimpleMatrix m {sm[0], sm[1], sm[2],
                    sm[3], sm[4], sm[5],
                    sm[6], sm[7], sm[8]};
    return m;
}

struct SimpleImageInfo {
    int width;
    int height;
    SkColorType colorType;
    SkAlphaType alphaType;
    // TODO color spaces?
};

SkImageInfo toSkImageInfo(const SimpleImageInfo& sii) {
    return SkImageInfo::Make(sii.width, sii.height, sii.colorType, sii.alphaType);
}

//========================================================================================
// Path things
//========================================================================================

// All these Apply* methods are simple wrappers to avoid returning an object.
// The default WASM bindings produce code that will leak if a return value
// isn't assigned to a JS variable and has delete() called on it.
// These Apply methods, combined with the smarter binding code allow for chainable
// commands that don't leak if the return value is ignored (i.e. when used intuitively).

void ApplyAddArc(SkPath& orig, const SkRect& oval, SkScalar startAngle, SkScalar sweepAngle) {
    orig.addArc(oval, startAngle, sweepAngle);
}

void ApplyAddPath(SkPath& orig, const SkPath& newPath,
                   SkScalar scaleX, SkScalar skewX,  SkScalar transX,
                   SkScalar skewY,  SkScalar scaleY, SkScalar transY,
                   SkScalar pers0, SkScalar pers1, SkScalar pers2,
                   bool extendPath) {
    SkMatrix m = SkMatrix::MakeAll(scaleX, skewX , transX,
                                   skewY , scaleY, transY,
                                   pers0 , pers1 , pers2);
    orig.addPath(newPath, m, extendPath ? SkPath::kExtend_AddPathMode :
                                          SkPath::kAppend_AddPathMode);
}

void ApplyAddRect(SkPath& path, SkScalar left, SkScalar top,
                  SkScalar right, SkScalar bottom, bool ccw) {
    path.addRect(left, top, right, bottom,
                 ccw ? SkPath::Direction::kCCW_Direction :
                 SkPath::Direction::kCW_Direction);
}

void ApplyArcTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                SkScalar radius) {
    p.arcTo(x1, y1, x2, y2, radius);
}

void ApplyArcToAngle(SkPath& p, SkRect& oval, SkScalar startAngle, SkScalar sweepAngle, bool forceMoveTo) {
    p.arcTo(oval, startAngle, sweepAngle, forceMoveTo);
}

void ApplyClose(SkPath& p) {
    p.close();
}

void ApplyConicTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                  SkScalar w) {
    p.conicTo(x1, y1, x2, y2, w);
}

void ApplyCubicTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2,
                  SkScalar x3, SkScalar y3) {
    p.cubicTo(x1, y1, x2, y2, x3, y3);
}

void ApplyLineTo(SkPath& p, SkScalar x, SkScalar y) {
    p.lineTo(x, y);
}

void ApplyMoveTo(SkPath& p, SkScalar x, SkScalar y) {
    p.moveTo(x, y);
}

void ApplyQuadTo(SkPath& p, SkScalar x1, SkScalar y1, SkScalar x2, SkScalar y2) {
    p.quadTo(x1, y1, x2, y2);
}

void ApplyTransform(SkPath& orig,
                    SkScalar scaleX, SkScalar skewX,  SkScalar transX,
                    SkScalar skewY,  SkScalar scaleY, SkScalar transY,
                    SkScalar pers0, SkScalar pers1, SkScalar pers2) {
    SkMatrix m = SkMatrix::MakeAll(scaleX, skewX , transX,
                                   skewY , scaleY, transY,
                                   pers0 , pers1 , pers2);
    orig.transform(m);
}

bool EMSCRIPTEN_KEEPALIVE ApplySimplify(SkPath& path) {
    return Simplify(path, &path);
}

bool EMSCRIPTEN_KEEPALIVE ApplyPathOp(SkPath& pathOne, const SkPath& pathTwo, SkPathOp op) {
    return Op(pathOne, pathTwo, op, &pathOne);
}

JSString EMSCRIPTEN_KEEPALIVE ToSVGString(const SkPath& path) {
    SkString s;
    SkParsePath::ToSVGString(path, &s);
    return emscripten::val(s.c_str());
}

SkPathOrNull EMSCRIPTEN_KEEPALIVE MakePathFromSVGString(std::string str) {
    SkPath path;
    if (SkParsePath::FromSVGString(str.c_str(), &path)) {
        return emscripten::val(path);
    }
    return emscripten::val::null();
}

SkPathOrNull EMSCRIPTEN_KEEPALIVE MakePathFromOp(const SkPath& pathOne, const SkPath& pathTwo, SkPathOp op) {
    SkPath out;
    if (Op(pathOne, pathTwo, op, &out)) {
        return emscripten::val(out);
    }
    return emscripten::val::null();
}

SkPath EMSCRIPTEN_KEEPALIVE CopyPath(const SkPath& a) {
    SkPath copy(a);
    return copy;
}

bool EMSCRIPTEN_KEEPALIVE Equals(const SkPath& a, const SkPath& b) {
    return a == b;
}

//========================================================================================
// Path Effects
//========================================================================================

bool ApplyDash(SkPath& path, SkScalar on, SkScalar off, SkScalar phase) {
    SkScalar intervals[] = { on, off };
    auto pe = SkDashPathEffect::Make(intervals, 2, phase);
    if (!pe) {
        SkDebugf("Invalid args to dash()\n");
        return false;
    }
    SkStrokeRec rec(SkStrokeRec::InitStyle::kHairline_InitStyle);
    if (pe->filterPath(&path, path, &rec, nullptr)) {
        return true;
    }
    SkDebugf("Could not make dashed path\n");
    return false;
}

bool ApplyTrim(SkPath& path, SkScalar startT, SkScalar stopT, bool isComplement) {
    auto mode = isComplement ? SkTrimPathEffect::Mode::kInverted : SkTrimPathEffect::Mode::kNormal;
    auto pe = SkTrimPathEffect::Make(startT, stopT, mode);
    if (!pe) {
        SkDebugf("Invalid args to trim(): startT and stopT must be in [0,1]\n");
        return false;
    }
    SkStrokeRec rec(SkStrokeRec::InitStyle::kHairline_InitStyle);
    if (pe->filterPath(&path, path, &rec, nullptr)) {
        return true;
    }
    SkDebugf("Could not trim path\n");
    return false;
}

struct StrokeOpts {
    // Default values are set in interface.js which allows clients
    // to set any number of them. Otherwise, the binding code complains if
    // any are omitted.
    SkScalar width;
    SkScalar miter_limit;
    SkPaint::Join join;
    SkPaint::Cap cap;
    float precision;
};

bool ApplyStroke(SkPath& path, StrokeOpts opts) {
    SkPaint p;
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeCap(opts.cap);
    p.setStrokeJoin(opts.join);
    p.setStrokeWidth(opts.width);
    p.setStrokeMiter(opts.miter_limit);

    return p.getFillPath(path, &path, nullptr, opts.precision);
}

// to map from raw memory to a uint8array
Uint8Array getSkDataBytes(const SkData *data) {
    return Uint8Array(typed_memory_view(data->size(), data->bytes()));
}

// These objects have private destructors / delete mthods - I don't think
// we need to do anything other than tell emscripten to do nothing.
namespace emscripten {
    namespace internal {
        template<typename ClassType>
        void raw_destructor(ClassType *);

        template<>
        void raw_destructor<SkData>(SkData *ptr) {
        }

        template<>
        void raw_destructor<SkTypeface>(SkTypeface *ptr) {
        }

        template<>
        void raw_destructor<SkVertices>(SkVertices *ptr) {
        }
    }
}

// Some timesignatures below have uintptr_t instead of a pointer to a primative
// type (e.g. SkScalar). This is necessary because we can't use "bind" (EMSCRIPTEN_BINDINGS)
// and pointers to primitive types (Only bound types like SkPoint). We could if we used
// cwrap (see https://becominghuman.ai/passing-and-returning-webassembly-array-parameters-a0f572c65d97)
// but that requires us to stick to C code and, AFAIK, doesn't allow us to return nice things like
// SkPath or SkCanvas.
//
// So, basically, if we are using C++ and EMSCRIPTEN_BINDINGS, we can't have primative pointers
// in our function type signatures. (this gives an error message like "Cannot call foo due to unbound
// types Pi, Pf").  But, we can just pretend they are numbers and cast them to be pointers and
// the compiler is happy.
EMSCRIPTEN_BINDINGS(Skia) {
#if SK_SUPPORT_GPU
    function("_getWebGLSurface", &getWebGLSurface, allow_raw_pointers());
    function("currentContext", &emscripten_webgl_get_current_context);
    function("setCurrentContext", &emscripten_webgl_make_context_current);
    constant("gpu", true);
#endif
    function("_decodeImage", optional_override([](uintptr_t /* uint8_t*  */ iptr,
                                                  size_t length)->sk_sp<SkImage> {
        uint8_t* imgData = reinterpret_cast<uint8_t*>(iptr);
        sk_sp<SkData> bytes = SkData::MakeWithoutCopy(imgData, length);
        return SkImage::MakeFromEncoded(bytes);
    }), allow_raw_pointers());
    function("_getRasterDirectSurface", optional_override([](const SimpleImageInfo ii,
                                                             uintptr_t /* uint8_t*  */ pPtr,
                                                             size_t rowBytes)->sk_sp<SkSurface> {
        uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
        SkImageInfo imageInfo = toSkImageInfo(ii);
        return SkSurface::MakeRasterDirect(imageInfo, pixels, rowBytes, nullptr);
    }), allow_raw_pointers());
    function("_getRasterN32PremulSurface", optional_override([](int width, int height)->sk_sp<SkSurface> {
        return SkSurface::MakeRasterN32Premul(width, height, nullptr);
    }), allow_raw_pointers());

    function("getSkDataBytes", &getSkDataBytes, allow_raw_pointers());
    function("MakeSkCornerPathEffect", &SkCornerPathEffect::Make, allow_raw_pointers());
    function("MakeSkDiscretePathEffect", &SkDiscretePathEffect::Make, allow_raw_pointers());
    function("MakeBlurMaskFilter", optional_override([](SkBlurStyle style, SkScalar sigma, bool respectCTM)->sk_sp<SkMaskFilter> {
        // Adds a little helper because emscripten doesn't expose default params.
        return SkMaskFilter::MakeBlur(style, sigma, respectCTM);
    }), allow_raw_pointers());
    function("MakePathFromOp", &MakePathFromOp);
    function("MakePathFromSVGString", &MakePathFromSVGString);

    // These won't be called directly, there's a JS helper to deal with typed arrays.
    function("_MakeSkDashPathEffect", optional_override([](uintptr_t /* float* */ cptr, int count, SkScalar phase)->sk_sp<SkPathEffect> {
        // See comment above for uintptr_t explanation
        const float* intervals = reinterpret_cast<const float*>(cptr);
        return SkDashPathEffect::Make(intervals, count, phase);
    }), allow_raw_pointers());
    function("_MakeImage", optional_override([](SimpleImageInfo ii,
                                                uintptr_t /* uint8_t*  */ pPtr, int plen,
                                                size_t rowBytes)->sk_sp<SkImage> {
        // See comment above for uintptr_t explanation
        uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
        SkImageInfo info = toSkImageInfo(ii);
        sk_sp<SkData> pixelData = SkData::MakeFromMalloc(pixels, plen);

        return SkImage::MakeRasterData(info, pixelData, rowBytes);
    }), allow_raw_pointers());
    // Allow localMatrix to be optional, so we have 2 declarations of these shaders
    function("_MakeImageShader", optional_override([](sk_sp<SkImage> img,
                                SkShader::TileMode tx, SkShader::TileMode ty,
                                bool clampAsIfUnpremul)->sk_sp<SkShader> {
        return SkImageShader::Make(img, tx, ty, nullptr, clampAsIfUnpremul);
    }), allow_raw_pointers());
    function("_MakeImageShader", optional_override([](sk_sp<SkImage> img,
                                SkShader::TileMode tx, SkShader::TileMode ty,
                                bool clampAsIfUnpremul, const SimpleMatrix& lm)->sk_sp<SkShader> {
        SkMatrix localMatrix = toSkMatrix(lm);

        return SkImageShader::Make(img, tx, ty, &localMatrix, clampAsIfUnpremul);
    }), allow_raw_pointers());
    function("_MakeLinearGradientShader", optional_override([](SkPoint start, SkPoint end,
                                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                                int count, SkShader::TileMode mode, uint32_t flags)->sk_sp<SkShader> {
        SkPoint points[] = { start, end };
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        return SkGradientShader::MakeLinear(points, colors, positions, count,
                                            mode, flags, nullptr);
    }), allow_raw_pointers());
    function("_MakeLinearGradientShader", optional_override([](SkPoint start, SkPoint end,
                                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                                int count, SkShader::TileMode mode, uint32_t flags,
                                const SimpleMatrix& lm)->sk_sp<SkShader> {
        SkPoint points[] = { start, end };
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        SkMatrix localMatrix = toSkMatrix(lm);

        return SkGradientShader::MakeLinear(points, colors, positions, count,
                                            mode, flags, &localMatrix);
    }), allow_raw_pointers());
    function("_MakeRadialGradientShader", optional_override([](SkPoint center, SkScalar radius,
                                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                                int count, SkShader::TileMode mode, uint32_t flags)->sk_sp<SkShader> {
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        return SkGradientShader::MakeRadial(center, radius, colors, positions, count,
                                            mode, flags, nullptr);
    }), allow_raw_pointers());
    function("_MakeRadialGradientShader", optional_override([](SkPoint center, SkScalar radius,
                                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                                int count, SkShader::TileMode mode, uint32_t flags,
                                const SimpleMatrix& lm)->sk_sp<SkShader> {
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        SkMatrix localMatrix = toSkMatrix(lm);
        return SkGradientShader::MakeRadial(center, radius, colors, positions, count,
                                            mode, flags, &localMatrix);
    }), allow_raw_pointers());
    function("_MakeTwoPointConicalGradientShader", optional_override([](
                SkPoint start, SkScalar startRadius,
                SkPoint end, SkScalar endRadius,
                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                int count, SkShader::TileMode mode, uint32_t flags)->sk_sp<SkShader> {
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        return SkGradientShader::MakeTwoPointConical(start, startRadius, end, endRadius,
                                                     colors, positions, count, mode,
                                                     flags, nullptr);
    }), allow_raw_pointers());
    function("_MakeTwoPointConicalGradientShader", optional_override([](
                SkPoint start, SkScalar startRadius,
                SkPoint end, SkScalar endRadius,
                uintptr_t /* SkColor*  */ cPtr, uintptr_t /* SkScalar*  */ pPtr,
                int count, SkShader::TileMode mode, uint32_t flags,
                const SimpleMatrix& lm)->sk_sp<SkShader> {
        // See comment above for uintptr_t explanation
        const SkColor*  colors    = reinterpret_cast<const SkColor*> (cPtr);
        const SkScalar* positions = reinterpret_cast<const SkScalar*>(pPtr);

        SkMatrix localMatrix = toSkMatrix(lm);
        return SkGradientShader::MakeTwoPointConical(start, startRadius, end, endRadius,
                                                     colors, positions, count, mode,
                                                     flags, &localMatrix);
    }), allow_raw_pointers());

    function("_MakeSkVertices", optional_override([](SkVertices::VertexMode mode, int vertexCount,
                                uintptr_t /* SkPoint*     */ pPtr,  uintptr_t /* SkPoint*     */ tPtr,
                                uintptr_t /* SkColor*     */ cPtr,
                                uintptr_t /* BoneIndices* */ biPtr, uintptr_t /* BoneWeights* */ bwPtr,
                                int indexCount,                     uintptr_t /* uint16_t  *  */ iPtr)->sk_sp<SkVertices> {
        // See comment above for uintptr_t explanation
        const SkPoint* positions       = reinterpret_cast<const SkPoint*>(pPtr);
        const SkPoint* texs            = reinterpret_cast<const SkPoint*>(tPtr);
        const SkColor* colors          = reinterpret_cast<const SkColor*>(cPtr);
        const BoneIndices* boneIndices = reinterpret_cast<const BoneIndices*>(biPtr);
        const BoneWeights* boneWeights = reinterpret_cast<const BoneWeights*>(bwPtr);
        const uint16_t* indices        = reinterpret_cast<const uint16_t*>(iPtr);

        return SkVertices::MakeCopy(mode, vertexCount, positions, texs, colors,
                                    boneIndices, boneWeights, indexCount, indices);
    }), allow_raw_pointers());

    class_<SkCanvas>("SkCanvas")
        .constructor<>()
        .function("clear", optional_override([](SkCanvas& self, JSColor color)->void {
            // JS side gives us a signed int instead of an unsigned int for color
            // Add a optional_override to change it out.
            self.clear(SkColor(color));
        }))
        .function("clipPath", select_overload<void (const SkPath&, SkClipOp, bool)>(&SkCanvas::clipPath))
        .function("clipRect", select_overload<void (const SkRect&, SkClipOp, bool)>(&SkCanvas::clipRect))
        .function("concat", optional_override([](SkCanvas& self, const SimpleMatrix& m) {
            self.concat(toSkMatrix(m));
        }))
        .function("drawImage", select_overload<void (const sk_sp<SkImage>&, SkScalar, SkScalar, const SkPaint*)>(&SkCanvas::drawImage), allow_raw_pointers())
        .function("drawImageRect", optional_override([](SkCanvas& self, const sk_sp<SkImage>& image,
                                                        SkRect src, SkRect dst,
                                                        const SkPaint* paint, bool fastSample)->void {
            self.drawImageRect(image, src, dst, paint,
                               fastSample ? SkCanvas::kFast_SrcRectConstraint :
                                            SkCanvas::kStrict_SrcRectConstraint);
        }), allow_raw_pointers())
        .function("drawPaint", &SkCanvas::drawPaint)
        .function("drawPath", &SkCanvas::drawPath)
        .function("drawRect", &SkCanvas::drawRect)
        .function("drawShadow", optional_override([](SkCanvas& self, const SkPath& path,
                                                     const SkPoint3& zPlaneParams,
                                                     const SkPoint3& lightPos, SkScalar lightRadius,
                                                     JSColor ambientColor, JSColor spotColor,
                                                     uint32_t flags) {
            SkShadowUtils::DrawShadow(&self, path, zPlaneParams, lightPos, lightRadius,
                                      SkColor(ambientColor), SkColor(spotColor), flags);
        }))
        .function("drawText", optional_override([](SkCanvas& self, std::string text, SkScalar x,
                                                   SkScalar y, const SkPaint& p) {
            // TODO(kjlubick): This does not work well for non-ascii
            // Need to maybe add a helper in interface.js that supports UTF-8
            // Otherwise, go with std::wstring and set UTF-32 encoding.
            self.drawText(text.c_str(), text.length(), x, y, p);
        }))
        .function("drawVertices", select_overload<void (const sk_sp<SkVertices>&, SkBlendMode, const SkPaint&)>(&SkCanvas::drawVertices))
        .function("flush", &SkCanvas::flush)
        .function("getTotalMatrix", optional_override([](const SkCanvas& self)->SimpleMatrix {
            SkMatrix m = self.getTotalMatrix();
            return toSimpleSkMatrix(m);
        }))
        .function("_readPixels", optional_override([](SkCanvas& self, SimpleImageInfo di,
                                                      uintptr_t /* uint8_t* */ pPtr,
                                                      size_t dstRowBytes, int srcX, int srcY) {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo dstInfo = toSkImageInfo(di);

            return self.readPixels(dstInfo, pixels, dstRowBytes, srcX, srcY);
        }))
        .function("restore", &SkCanvas::restore)
        .function("rotate", select_overload<void (SkScalar, SkScalar, SkScalar)>(&SkCanvas::rotate))
        .function("save", &SkCanvas::save)
        .function("scale", &SkCanvas::scale)
        .function("skew", &SkCanvas::skew)
        .function("translate", &SkCanvas::translate)
        .function("_writePixels", optional_override([](SkCanvas& self, SimpleImageInfo di,
                                                       uintptr_t /* uint8_t* */ pPtr,
                                                       size_t srcRowBytes, int dstX, int dstY) {
            uint8_t* pixels = reinterpret_cast<uint8_t*>(pPtr);
            SkImageInfo dstInfo = toSkImageInfo(di);

            return self.writePixels(dstInfo, pixels, srcRowBytes, dstX, dstY);
        }))
        ;

    class_<SkData>("SkData")
        .smart_ptr<sk_sp<SkData>>("sk_sp<SkData>>")
        .function("size", &SkData::size);

    class_<SkFont>("SkFont")
        .constructor<>()
        .constructor<sk_sp<SkTypeface>>()
        .constructor<sk_sp<SkTypeface>, SkScalar>()
        .constructor<sk_sp<SkTypeface>, SkScalar, SkScalar, SkScalar>()
        .function("getScaleX", &SkFont::getScaleX)
        .function("getSize", &SkFont::getSize)
        .function("getSkewX", &SkFont::getSkewX)
        .function("getTypeface", &SkFont::getTypeface, allow_raw_pointers())
        .function("measureText", optional_override([](SkFont& self, std::string text) {
            // TODO(kjlubick): This does not work well for non-ascii
            // Need to maybe add a helper in interface.js that supports UTF-8
            // Otherwise, go with std::wstring and set UTF-32 encoding.
            return self.measureText(text.c_str(), text.length(), SkTextEncoding::kUTF8);
        }))
        .function("setScaleX", &SkFont::setScaleX)
        .function("setSize", &SkFont::setSize)
        .function("setSkewX", &SkFont::setSkewX)
        .function("setTypeface", &SkFont::setTypeface, allow_raw_pointers());

    class_<SkFontMgr>("SkFontMgr")
        .smart_ptr<sk_sp<SkFontMgr>>("sk_sp<SkFontMgr>")
        .class_function("RefDefault", &SkFontMgr::RefDefault)
#ifdef SK_DEBUG
        .function("dumpFamilies", optional_override([](SkFontMgr& self) {
            int numFam = self.countFamilies();
            SkDebugf("There are %d font families\n");
            for (int i = 0 ; i< numFam; i++) {
                SkString s;
                self.getFamilyName(i, &s);
                SkDebugf("\t%s", s.c_str());
            }
        }))
#endif
        .function("countFamilies", &SkFontMgr::countFamilies)
        .function("_makeTypefaceFromData", optional_override([](SkFontMgr& self,
                                                uintptr_t /* uint8_t*  */ fPtr,
                                                int flen)->sk_sp<SkTypeface> {
        // See comment above for uintptr_t explanation
        uint8_t* font = reinterpret_cast<uint8_t*>(fPtr);
        sk_sp<SkData> fontData = SkData::MakeFromMalloc(font, flen);

        return self.makeFromData(fontData);
    }), allow_raw_pointers());

    class_<SkImage>("SkImage")
        .smart_ptr<sk_sp<SkImage>>("sk_sp<SkImage>")
        .function("height", &SkImage::height)
        .function("width", &SkImage::width)
        .function("_encodeToData", select_overload<sk_sp<SkData>()const>(&SkImage::encodeToData))
        .function("_encodeToDataWithFormat", select_overload<sk_sp<SkData>(SkEncodedImageFormat encodedImageFormat, int quality)const>(&SkImage::encodeToData));

    class_<SkMaskFilter>("SkMaskFilter")
        .smart_ptr<sk_sp<SkMaskFilter>>("sk_sp<SkMaskFilter>");

    class_<SkPaint>("SkPaint")
        .constructor<>()
        .function("copy", optional_override([](const SkPaint& self)->SkPaint {
            SkPaint p(self);
            return p;
        }))
        .function("getBlendMode", &SkPaint::getBlendMode)
        .function("getColor", optional_override([](SkPaint& self)->JSColor {
            // JS side gives us a signed int instead of an unsigned int for color
            // Add a optional_override to change it out.
            return JSColor(self.getColor());
        }))
        .function("getFilterQuality", &SkPaint::getFilterQuality)
        .function("getStrokeCap", &SkPaint::getStrokeCap)
        .function("getStrokeJoin", &SkPaint::getStrokeJoin)
        .function("getStrokeMiter", &SkPaint::getStrokeMiter)
        .function("getStrokeWidth", &SkPaint::getStrokeWidth)
        .function("getTextSize", &SkPaint::getTextSize)
        .function("setAntiAlias", &SkPaint::setAntiAlias)
        .function("setBlendMode", &SkPaint::setBlendMode)
        .function("setColor", optional_override([](SkPaint& self, JSColor color)->void {
            // JS side gives us a signed int instead of an unsigned int for color
            // Add a optional_override to change it out.
            self.setColor(SkColor(color));
        }))
        .function("setFilterQuality", &SkPaint::setFilterQuality)
        .function("setMaskFilter", &SkPaint::setMaskFilter)
        .function("setPathEffect", &SkPaint::setPathEffect)
        .function("setShader", &SkPaint::setShader)
        .function("setStrokeCap", &SkPaint::setStrokeCap)
        .function("setStrokeJoin", &SkPaint::setStrokeJoin)
        .function("setStrokeMiter", &SkPaint::setStrokeMiter)
        .function("setStrokeWidth", &SkPaint::setStrokeWidth)
        .function("setStyle", &SkPaint::setStyle)
        .function("setTypeface", &SkPaint::setTypeface)
        .function("setTextSize", &SkPaint::setTextSize);

    class_<SkPathEffect>("SkPathEffect")
        .smart_ptr<sk_sp<SkPathEffect>>("sk_sp<SkPathEffect>");

    class_<SkPath>("SkPath")
        .constructor<>()
        .constructor<const SkPath&>()
        .function("_addArc", &ApplyAddArc)
        // interface.js has 3 overloads of addPath
        .function("_addPath", &ApplyAddPath)
        // interface.js has 4 overloads of addRect
        .function("_addRect", &ApplyAddRect)
        .function("_arcTo", &ApplyArcTo)
        .function("_arcTo", &ApplyArcToAngle)
        .function("_close", &ApplyClose)
        .function("_conicTo", &ApplyConicTo)
        .function("countPoints", &SkPath::countPoints)
        .function("contains", &SkPath::contains)
        .function("_cubicTo", &ApplyCubicTo)
        .function("getPoint", &SkPath::getPoint)
        .function("isEmpty",  &SkPath::isEmpty)
        .function("isVolatile", &SkPath::isVolatile)
        .function("_lineTo", &ApplyLineTo)
        .function("_moveTo", &ApplyMoveTo)
        .function("_quadTo", &ApplyQuadTo)
        .function("setIsVolatile", &SkPath::setIsVolatile)
        .function("_transform", select_overload<void(SkPath&, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar, SkScalar)>(&ApplyTransform))

        // PathEffects
        .function("_dash", &ApplyDash)
        .function("_trim", &ApplyTrim)
        .function("_stroke", &ApplyStroke)

        // PathOps
        .function("_simplify", &ApplySimplify)
        .function("_op", &ApplyPathOp)

        // Exporting
        .function("toSVGString", &ToSVGString)

        .function("setFillType", &SkPath::setFillType)
        .function("getFillType", &SkPath::getFillType)
        .function("getBounds", &SkPath::getBounds)
        .function("computeTightBounds", &SkPath::computeTightBounds)
        .function("equals", &Equals)
        .function("copy", &CopyPath)
#ifdef SK_DEBUG
        .function("dump", select_overload<void() const>(&SkPath::dump))
        .function("dumpHex", select_overload<void() const>(&SkPath::dumpHex))
#endif
        ;

    class_<SkShader>("SkShader")
        .smart_ptr<sk_sp<SkShader>>("sk_sp<SkShader>");

    class_<SkSurface>("SkSurface")
        .smart_ptr<sk_sp<SkSurface>>("sk_sp<SkSurface>")
        .function("width", &SkSurface::width)
        .function("height", &SkSurface::height)
        .function("_flush", &SkSurface::flush)
        .function("makeImageSnapshot", select_overload<sk_sp<SkImage>()>(&SkSurface::makeImageSnapshot))
        .function("makeImageSnapshot", select_overload<sk_sp<SkImage>(const SkIRect& bounds)>(&SkSurface::makeImageSnapshot))
        .function("getCanvas", &SkSurface::getCanvas, allow_raw_pointers());

    class_<SkTypeface>("SkTypeface")
        .smart_ptr<sk_sp<SkTypeface>>("sk_sp<SkTypeface>");

    class_<SkVertices>("SkVertices")
        .smart_ptr<sk_sp<SkVertices>>("sk_sp<SkVertices>")
        .function("_applyBones", optional_override([](SkVertices& self, uintptr_t /* Bone* */ bptr, int boneCount)->sk_sp<SkVertices> {
            // See comment above for uintptr_t explanation
            const Bone* bones = reinterpret_cast<const Bone*>(bptr);
            return self.applyBones(bones, boneCount);
        }))
        .function("bounds", &SkVertices::bounds)
        .function("mode", &SkVertices::mode)
        .function("uniqueID", &SkVertices::uniqueID)
#ifdef SK_DEBUG
        .function("dumpPositions", optional_override([](SkVertices& self)->void {
            auto pos = self.positions();
            for(int i = 0; i< self.vertexCount(); i++) {
                SkDebugf("position[%d] = (%f, %f)\n", i, pos[i].x(), pos[i].y());
            }
        }))
#endif
        .function("vertexCount", &SkVertices::vertexCount);

    enum_<SkAlphaType>("AlphaType")
        .value("Opaque",   SkAlphaType::kOpaque_SkAlphaType)
        .value("Premul",   SkAlphaType::kPremul_SkAlphaType)
        .value("Unpremul", SkAlphaType::kUnpremul_SkAlphaType);

    enum_<SkBlendMode>("BlendMode")
        .value("Clear",      SkBlendMode::kClear)
        .value("Src",        SkBlendMode::kSrc)
        .value("Dst",        SkBlendMode::kDst)
        .value("SrcOver",    SkBlendMode::kSrcOver)
        .value("DstOver",    SkBlendMode::kDstOver)
        .value("SrcIn",      SkBlendMode::kSrcIn)
        .value("DstIn",      SkBlendMode::kDstIn)
        .value("SrcOut",     SkBlendMode::kSrcOut)
        .value("DstOut",     SkBlendMode::kDstOut)
        .value("SrcATop",    SkBlendMode::kSrcATop)
        .value("DstATop",    SkBlendMode::kDstATop)
        .value("Xor",        SkBlendMode::kXor)
        .value("Plus",       SkBlendMode::kPlus)
        .value("Modulate",   SkBlendMode::kModulate)
        .value("Screen",     SkBlendMode::kScreen)
        .value("Overlay",    SkBlendMode::kOverlay)
        .value("Darken",     SkBlendMode::kDarken)
        .value("Lighten",    SkBlendMode::kLighten)
        .value("ColorDodge", SkBlendMode::kColorDodge)
        .value("ColorBurn",  SkBlendMode::kColorBurn)
        .value("HardLight",  SkBlendMode::kHardLight)
        .value("SoftLight",  SkBlendMode::kSoftLight)
        .value("Difference", SkBlendMode::kDifference)
        .value("Exclusion",  SkBlendMode::kExclusion)
        .value("Multiply",   SkBlendMode::kMultiply)
        .value("Hue",        SkBlendMode::kHue)
        .value("Saturation", SkBlendMode::kSaturation)
        .value("Color",      SkBlendMode::kColor)
        .value("Luminosity", SkBlendMode::kLuminosity);

    enum_<SkBlurStyle>("BlurStyle")
        .value("Normal", SkBlurStyle::kNormal_SkBlurStyle)
        .value("Solid",  SkBlurStyle::kSolid_SkBlurStyle)
        .value("Outer",  SkBlurStyle::kOuter_SkBlurStyle)
        .value("Inner",  SkBlurStyle::kInner_SkBlurStyle);

    enum_<SkClipOp>("ClipOp")
        .value("Difference", SkClipOp::kDifference)
        .value("Intersect",  SkClipOp::kIntersect);

    enum_<SkColorType>("ColorType")
        .value("Alpha_8", SkColorType::kAlpha_8_SkColorType)
        .value("RGB_565", SkColorType::kRGB_565_SkColorType)
        .value("ARGB_4444", SkColorType::kARGB_4444_SkColorType)
        .value("RGBA_8888", SkColorType::kRGBA_8888_SkColorType)
        .value("RGB_888x", SkColorType::kRGB_888x_SkColorType)
        .value("BGRA_8888", SkColorType::kBGRA_8888_SkColorType)
        .value("RGBA_1010102", SkColorType::kRGBA_1010102_SkColorType)
        .value("RGB_101010x", SkColorType::kRGB_101010x_SkColorType)
        .value("Gray_8", SkColorType::kGray_8_SkColorType)
        .value("RGBA_F16", SkColorType::kRGBA_F16_SkColorType)
        .value("RGBA_F32", SkColorType::kRGBA_F32_SkColorType);

    enum_<SkPath::FillType>("FillType")
        .value("Winding",           SkPath::FillType::kWinding_FillType)
        .value("EvenOdd",           SkPath::FillType::kEvenOdd_FillType)
        .value("InverseWinding",    SkPath::FillType::kInverseWinding_FillType)
        .value("InverseEvenOdd",    SkPath::FillType::kInverseEvenOdd_FillType);

    enum_<SkFilterQuality>("FilterQuality")
        .value("None",   SkFilterQuality::kNone_SkFilterQuality)
        .value("Low",    SkFilterQuality::kLow_SkFilterQuality)
        .value("Medium", SkFilterQuality::kMedium_SkFilterQuality)
        .value("High",   SkFilterQuality::kHigh_SkFilterQuality);

    enum_<SkEncodedImageFormat>("ImageFormat")
        .value("PNG",  SkEncodedImageFormat::kPNG)
        .value("JPEG", SkEncodedImageFormat::kJPEG);

    enum_<SkPaint::Style>("PaintStyle")
        .value("Fill",            SkPaint::Style::kFill_Style)
        .value("Stroke",          SkPaint::Style::kStroke_Style)
        .value("StrokeAndFill",   SkPaint::Style::kStrokeAndFill_Style);

    enum_<SkPathOp>("PathOp")
        .value("Difference",         SkPathOp::kDifference_SkPathOp)
        .value("Intersect",          SkPathOp::kIntersect_SkPathOp)
        .value("Union",              SkPathOp::kUnion_SkPathOp)
        .value("XOR",                SkPathOp::kXOR_SkPathOp)
        .value("ReverseDifference",  SkPathOp::kReverseDifference_SkPathOp);

    enum_<SkPaint::Cap>("StrokeCap")
        .value("Butt",   SkPaint::Cap::kButt_Cap)
        .value("Round",  SkPaint::Cap::kRound_Cap)
        .value("Square", SkPaint::Cap::kSquare_Cap);

    enum_<SkPaint::Join>("StrokeJoin")
        .value("Miter", SkPaint::Join::kMiter_Join)
        .value("Round", SkPaint::Join::kRound_Join)
        .value("Bevel", SkPaint::Join::kBevel_Join);

    value_object<StrokeOpts>("StrokeOpts")
        .field("width",       &StrokeOpts::width)
        .field("miter_limit", &StrokeOpts::miter_limit)
        .field("join",        &StrokeOpts::join)
        .field("cap",         &StrokeOpts::cap)
        .field("precision",   &StrokeOpts::precision);

    enum_<SkShader::TileMode>("TileMode")
        .value("Clamp",    SkShader::TileMode::kClamp_TileMode)
        .value("Repeat",   SkShader::TileMode::kRepeat_TileMode)
        .value("Mirror",   SkShader::TileMode::kMirror_TileMode)
        // Decal mode only works in the SW backend, not WebGl (yet).
        .value("Decal",    SkShader::TileMode::kDecal_TileMode);

    enum_<SkVertices::VertexMode>("VertexMode")
        .value("Triangles",       SkVertices::VertexMode::kTriangles_VertexMode)
        .value("TrianglesStrip",  SkVertices::VertexMode::kTriangleStrip_VertexMode)
        .value("TriangleFan",     SkVertices::VertexMode::kTriangleFan_VertexMode);


    // A value object is much simpler than a class - it is returned as a JS
    // object and does not require delete().
    // https://kripken.github.io/emscripten-site/docs/porting/connecting_cpp_and_javascript/embind.html#value-types
    value_object<SkRect>("SkRect")
        .field("fLeft",   &SkRect::fLeft)
        .field("fTop",    &SkRect::fTop)
        .field("fRight",  &SkRect::fRight)
        .field("fBottom", &SkRect::fBottom);

    value_object<SkIRect>("SkIRect")
        .field("fLeft",   &SkIRect::fLeft)
        .field("fTop",    &SkIRect::fTop)
        .field("fRight",  &SkIRect::fRight)
        .field("fBottom", &SkIRect::fBottom);

    value_object<SimpleImageInfo>("SkImageInfo")
        .field("width",     &SimpleImageInfo::width)
        .field("height",    &SimpleImageInfo::height)
        .field("colorType", &SimpleImageInfo::colorType)
        .field("alphaType", &SimpleImageInfo::alphaType);

    // SkPoints can be represented by [x, y]
    value_array<SkPoint>("SkPoint")
        .element(&SkPoint::fX)
        .element(&SkPoint::fY);

    // SkPoint3s can be represented by [x, y, z]
    value_array<SkPoint3>("SkPoint3")
        .element(&SkPoint3::fX)
        .element(&SkPoint3::fY)
        .element(&SkPoint3::fZ);

    // {"w": Number, "h", Number}
    value_object<SkSize>("SkSize")
        .field("w",   &SkSize::fWidth)
        .field("h",   &SkSize::fHeight);

    value_object<SkISize>("SkISize")
        .field("w",   &SkISize::fWidth)
        .field("h",   &SkISize::fHeight);

    // Allows clients to supply a 1D array of 9 elements and the bindings
    // will automatically turn it into a 3x3 2D matrix.
    // e.g. path.transform([0,1,2,3,4,5,6,7,8])
    // This is likely simpler for the client than exposing SkMatrix
    // directly and requiring them to do a lot of .delete().
    value_array<SimpleMatrix>("SkMatrix")
        .element(&SimpleMatrix::scaleX)
        .element(&SimpleMatrix::skewX)
        .element(&SimpleMatrix::transX)

        .element(&SimpleMatrix::skewY)
        .element(&SimpleMatrix::scaleY)
        .element(&SimpleMatrix::transY)

        .element(&SimpleMatrix::pers0)
        .element(&SimpleMatrix::pers1)
        .element(&SimpleMatrix::pers2);

    constant("TRANSPARENT", (JSColor) SK_ColorTRANSPARENT);
    constant("RED",         (JSColor) SK_ColorRED);
    constant("BLUE",        (JSColor) SK_ColorBLUE);
    constant("YELLOW",      (JSColor) SK_ColorYELLOW);
    constant("CYAN",        (JSColor) SK_ColorCYAN);
    constant("BLACK",       (JSColor) SK_ColorBLACK);
    constant("WHITE",       (JSColor) SK_ColorWHITE);
    // TODO(?)

}
