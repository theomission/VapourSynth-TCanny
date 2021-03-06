/*
**   VapourSynth port by HolyWu
**
**                 tcanny v1.0 for Avisynth 2.5.x
**
**   Copyright (C) 2009 Kevin Stone
**
**   This program is free software; you can redistribute it and/or modify
**   it under the terms of the GNU General Public License as published by
**   the Free Software Foundation; either version 2 of the License, or
**   (at your option) any later version.
**
**   This program is distributed in the hope that it will be useful,
**   but WITHOUT ANY WARRANTY; without even the implied warranty of
**   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**   GNU General Public License for more details.
**
**   You should have received a copy of the GNU General Public License
**   along with this program; if not, write to the Free Software
**   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vapoursynth/VapourSynth.h>
#include <vapoursynth/VSHelper.h>

#define M_PIF 3.14159265358979323846f

struct TCannyData {
    VSNodeRef * node;
    const VSVideoInfo * vi;
    float sigma, t_h, t_l, gmmax;
    int nms, mode, op;
    bool process[3];
    int grad, bins;
    float * weights;
    float magnitude;
    int peak;
    float lower[3], upper[3];
};

struct Stack {
    uint8_t * map;
    std::pair<int, int> * pos;
    int index;
};

static void push(Stack & s, const int x, const int y) {
    s.pos[++s.index].first = x;
    s.pos[s.index].second = y;
}

static std::pair<int, int> pop(Stack & s) {
    return s.pos[s.index--];
}

static float * gaussianWeights(const float sigma, int & rad) {
    const int dia = std::max(static_cast<int>(sigma * 3.f + 0.5f), 1) * 2 + 1;
    rad = dia >> 1;
    float * weights = vs_aligned_malloc<float>(dia * sizeof(float), 32);
    if (!weights)
        return nullptr;
    float sum = 0.f;
    for (int k = -rad; k <= rad; k++) {
        const float w = std::exp(-(k * k) / (2.f * sigma * sigma));
        weights[k + rad] = w;
        sum += w;
    }
    for (int k = 0; k < dia; k++)
        weights[k] /= sum;
    return weights;
}

template<typename T>
static void genConvV(const T * srcp, float * VS_RESTRICT dstp, const int width, const int height, const int stride, const int rad, const float * weights, const float offset) {
    weights += rad;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float sum = 0.f;
            for (int v = -rad; v <= rad; v++) {
                int yc = y + v;
                if (yc < 0)
                    yc = -yc;
                else if (yc >= height)
                    yc = 2 * (height - 1) - yc;
                sum += (srcp[x + yc * stride] + offset) * weights[v];
            }
            dstp[x] = sum;
        }
        dstp += stride;
    }
}

static void genConvH(const float * srcp, float * VS_RESTRICT dstp, const int width, const int height, const int stride, const int rad, const float * weights) {
    weights += rad;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float sum = 0.f;
            for (int v = -rad; v <= rad; v++) {
                int xc = x + v;
                if (xc < 0)
                    xc = -xc;
                else if (xc >= width)
                    xc = 2 * (width - 1) - xc;
                sum += srcp[xc] * weights[v];
            }
            dstp[x] = sum;
        }
        srcp += stride;
        dstp += stride;
    }
}

template<typename T>
static T getBin(const float dir, const int n) {
    const int bin = static_cast<int>(dir * (n / M_PIF) + 0.5f);
    return (bin >= n) ? 0 : bin;
}

template<>
float getBin<float>(const float dir, const int n) {
    const float bin = dir * (n / M_PIF);
    return (bin > static_cast<float>(n)) ? 0.f : bin;
}

static void gmDirImages(float * VS_RESTRICT srcp, float * VS_RESTRICT gimg, float * VS_RESTRICT dimg, const int width, const int height, const int stride,
                        const int nms, const int mode, const int op) {
    memset(gimg, 0, stride * height * sizeof(float));
    memset(dimg, 0, stride * height * sizeof(float));
    float * VS_RESTRICT srcpT = srcp + stride;
    float * VS_RESTRICT gmnT = gimg + stride;
    float * VS_RESTRICT dirT = dimg + stride;
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            float dx, dy;
            if (op == 0) {
                dx = srcpT[x + 1] - srcpT[x - 1];
                dy = srcpT[x - stride] - srcpT[x + stride];
            } else if (op == 1) {
                dx = (srcpT[x - stride + 1] + srcpT[x + 1] + srcpT[x + stride + 1] - srcpT[x - stride - 1] - srcpT[x - 1] - srcpT[x + stride - 1]) / 2.f;
                dy = (srcpT[x - stride - 1] + srcpT[x - stride] + srcpT[x - stride + 1] - srcpT[x + stride - 1] - srcpT[x + stride] - srcpT[x + stride + 1]) / 2.f;
            } else {
                dx = srcpT[x - stride + 1] + 2.f * srcpT[x + 1] + srcpT[x + stride + 1] - srcpT[x - stride - 1] - 2.f * srcpT[x - 1] - srcpT[x + stride - 1];
                dy = srcpT[x - stride - 1] + 2.f * srcpT[x - stride] + srcpT[x - stride + 1] - srcpT[x + stride - 1] - 2.f * srcpT[x + stride] - srcpT[x + stride + 1];
            }
            gmnT[x] = std::sqrt(dx * dx + dy * dy);
            if (mode == 1)
                continue;
            const float dr = std::atan2(dy, dx);
            dirT[x] = dr + (dr < 0.f ? M_PIF : 0.f);
        }
        srcpT += stride;
        gmnT += stride;
        dirT += stride;
    }
    memcpy(srcp, gimg, stride * height * sizeof(float));
    if (mode & 1)
        return;
    const int offTable[4] = { 1, -stride + 1, -stride, -stride - 1 };
    srcpT = srcp + stride;
    gmnT = gimg + stride;
    dirT = dimg + stride;
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            const float dir = dirT[x];
            if (nms & 1) {
                const int off = offTable[getBin<int>(dir, 4)];
                if (gmnT[x] >= std::max(gmnT[x + off], gmnT[x - off]))
                    continue;
            }
            if (nms & 2) {
                const int c = static_cast<int>(dir * (4.f / M_PIF));
                float val1, val2;
                if (c == 0 || c >= 4) {
                    const float h = std::tan(dir);
                    val1 = (1.f - h) * gmnT[x + 1] + h * gmnT[x - stride + 1];
                    val2 = (1.f - h) * gmnT[x - 1] + h * gmnT[x + stride - 1];
                } else if (c == 1) {
                    const float w = 1.f / std::tan(dir);
                    val1 = (1.f - w) * gmnT[x - stride] + w * gmnT[x - stride + 1];
                    val2 = (1.f - w) * gmnT[x + stride] + w * gmnT[x + stride - 1];
                } else if (c == 2) {
                    const float w = 1.f / std::tan(M_PIF - dir);
                    val1 = (1.f - w) * gmnT[x - stride] + w * gmnT[x - stride - 1];
                    val2 = (1.f - w) * gmnT[x + stride] + w * gmnT[x + stride + 1];
                } else {
                    const float h = std::tan(M_PIF - dir);
                    val1 = (1.f - h) * gmnT[x - 1] + h * gmnT[x - stride - 1];
                    val2 = (1.f - h) * gmnT[x + 1] + h * gmnT[x + stride + 1];
                }
                if (gmnT[x] >= std::max(val1, val2))
                    continue;
            }
            srcpT[x] = -FLT_MAX;
        }
        srcpT += stride;
        gmnT += stride;
        dirT += stride;
    }
}

static void hystersis(float * VS_RESTRICT srcp, Stack & VS_RESTRICT stack, const int width, const int height, const int stride, const float t_h, const float t_l) {
    memset(stack.map, 0, width * height);
    stack.index = -1;
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            if (srcp[x + y * stride] < t_h || stack.map[x + y * width])
                continue;
            srcp[x + y * stride] = FLT_MAX;
            stack.map[x + y * width] = UINT8_MAX;
            push(stack, x, y);
            while (stack.index > -1) {
                const std::pair<int, int> pos = pop(stack);
                const int xMin = (pos.first > 1) ? pos.first - 1 : 1;
                const int xMax = (pos.first < width - 2) ? pos.first + 1 : pos.first;
                const int yMin = (pos.second > 1) ? pos.second - 1 : 1;
                const int yMax = (pos.second < height - 2) ? pos.second + 1 : pos.second;
                for (int yy = yMin; yy <= yMax; yy++) {
                    for (int xx = xMin; xx <= xMax; xx++) {
                        if (srcp[xx + yy * stride] > t_l && !stack.map[xx + yy * width]) {
                            srcp[xx + yy * stride] = FLT_MAX;
                            stack.map[xx + yy * width] = UINT8_MAX;
                            push(stack, xx, yy);
                        }
                    }
                }
            }
        }
    }
}

template<typename T>
static void outputGB(const float * srcp, T * VS_RESTRICT dstp, const int width, const int height, const int stride,
                     const int peak, const float offset, const float lower, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = std::min(std::max(static_cast<int>(srcp[x] + 0.5f), 0), peak);
        srcp += stride;
        dstp += stride;
    }
}

template<>
void outputGB<float>(const float * srcp, float * VS_RESTRICT dstp, const int width, const int height, const int stride,
                     const int peak, const float offset, const float lower, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = std::min(std::max(srcp[x] - offset, lower), upper);
        srcp += stride;
        dstp += stride;
    }
}

template<typename T>
static void binarizeCE(const float * srcp, T * VS_RESTRICT dstp, const int width, const int height, const int stride, const float t_h, const T peak, const float lower, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = (srcp[x] >= t_h) ? peak : 0;
        srcp += stride;
        dstp += stride;
    }
}

template<>
void binarizeCE<float>(const float * srcp, float * VS_RESTRICT dstp, const int width, const int height, const int stride,
                       const float t_h, const float peak, const float lower, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = (srcp[x] >= t_h) ? upper : lower;
        srcp += stride;
        dstp += stride;
    }
}

template<typename T>
static void discretizeGM(const float * gimg, T * VS_RESTRICT dstp, const int width, const int height, const int stride,
                         const float magnitude, const int peak, const float offset, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = std::min(static_cast<int>(gimg[x] * magnitude + 0.5f), peak);
        gimg += stride;
        dstp += stride;
    }
}

template<>
void discretizeGM<float>(const float * gimg, float * VS_RESTRICT dstp, const int width, const int height, const int stride,
                         const float magnitude, const int peak, const float offset, const float upper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = std::min(gimg[x] * magnitude - offset, upper);
        gimg += stride;
        dstp += stride;
    }
}

template<typename T>
static void discretizeDM_T(const float * srcp, const float * dimg, T * VS_RESTRICT dstp, const int width, const int height, const int stride,
                           const float t_h, const int bins, const float offset, const float lower) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = (srcp[x] >= t_h) ? getBin<T>(dimg[x], bins) : 0;
        srcp += stride;
        dimg += stride;
        dstp += stride;
    }
}

template<>
void discretizeDM_T<float>(const float * srcp, const float * dimg, float * VS_RESTRICT dstp, const int width, const int height, const int stride,
                           const float t_h, const int bins, const float offset, const float lower) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = (srcp[x] >= t_h) ? getBin<float>(dimg[x], bins) - offset : lower;
        srcp += stride;
        dimg += stride;
        dstp += stride;
    }
}

template<typename T>
static void discretizeDM(const float * dimg, T * VS_RESTRICT dstp, const int width, const int height, const int stride, const int bins, const float offset) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = getBin<T>(dimg[x], bins);
        dimg += stride;
        dstp += stride;
    }
}

template<>
void discretizeDM<float>(const float * dimg, float * VS_RESTRICT dstp, const int width, const int height, const int stride, const int bins, const float offset) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            dstp[x] = getBin<float>(dimg[x], bins) - offset;
        dimg += stride;
        dstp += stride;
    }
}

template<typename T>
static void TCanny(const VSFrameRef * src, VSFrameRef * dst, float * VS_RESTRICT fa[3], Stack & VS_RESTRICT stack, const TCannyData * d, const VSAPI * vsapi) {
    for (int plane = 0; plane < d->vi->format->numPlanes; plane++) {
        if (d->process[plane]) {
            const int width = vsapi->getFrameWidth(src, plane);
            const int height = vsapi->getFrameHeight(src, plane);
            const int stride = vsapi->getStride(src, plane) / sizeof(T);
            const T * srcp = reinterpret_cast<const T *>(vsapi->getReadPtr(src, plane));
            T * VS_RESTRICT dstp = reinterpret_cast<T *>(vsapi->getWritePtr(dst, plane));
            const float offset = (d->vi->format->sampleType == stInteger || plane == 0 || d->vi->format->colorFamily == cmRGB) ? 0.f : 0.5f;

            genConvV<T>(srcp, fa[1], width, height, stride, d->grad, d->weights, offset);
            genConvH(fa[1], fa[0], width, height, stride, d->grad, d->weights);

            if (d->mode != -1)
                gmDirImages(fa[0], fa[1], fa[2], width, height, stride, d->nms, d->mode, d->op);

            if (!(d->mode & 1))
                hystersis(fa[0], stack, width, height, stride, d->t_h, d->t_l);

            if (d->mode == -1)
                outputGB<T>(fa[0], dstp, width, height, stride, d->peak, offset, d->lower[plane], d->upper[plane]);
            else if (d->mode == 0)
                binarizeCE<T>(fa[0], dstp, width, height, stride, d->t_h, d->peak, d->lower[plane], d->upper[plane]);
            else if (d->mode == 1)
                discretizeGM<T>(fa[1], dstp, width, height, stride, d->magnitude, d->peak, offset, d->upper[plane]);
            else if (d->mode == 2)
                discretizeDM_T<T>(fa[0], fa[2], dstp, width, height, stride, d->t_h, d->bins, offset, d->lower[plane]);
            else
                discretizeDM<T>(fa[2], dstp, width, height, stride, d->bins, offset);
        }
    }
}

static void VS_CC tcannyInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    TCannyData * d = static_cast<TCannyData *>(*instanceData);
    vsapi->setVideoInfo(d->vi, 1, node);
}

static const VSFrameRef *VS_CC tcannyGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    const TCannyData * d = static_cast<const TCannyData *>(*instanceData);

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef * src = vsapi->getFrameFilter(n, d->node, frameCtx);
        const VSFrameRef * fr[] = { d->process[0] ? nullptr : src, d->process[1] ? nullptr : src, d->process[2] ? nullptr : src };
        const int pl[] = { 0, 1, 2 };
        VSFrameRef * dst = vsapi->newVideoFrame2(d->vi->format, d->vi->width, d->vi->height, fr, pl, src, core);

        float * fa[3];
        for (int i = 0; i < 3; i++) {
            fa[i] = vs_aligned_malloc<float>(vsapi->getStride(src, 0) / d->vi->format->bytesPerSample * d->vi->height * sizeof(float), 32);
            if (!fa[i]) {
                vsapi->setFilterError("TCanny: malloc failure (fa)", frameCtx);
                vsapi->freeFrame(src);
                vsapi->freeFrame(dst);
                return nullptr;
            }
        }

        Stack stack = {};
        if (!(d->mode & 1)) {
            stack.map = vs_aligned_malloc<uint8_t>(d->vi->width * d->vi->height, 32);
            stack.pos = vs_aligned_malloc<std::pair<int, int>>(d->vi->width * d->vi->height * sizeof(std::pair<int, int>), 32);
            if (!stack.map || !stack.pos) {
                vsapi->setFilterError("TCanny: malloc failure (stack)", frameCtx);
                vsapi->freeFrame(src);
                vsapi->freeFrame(dst);
                return nullptr;
            }
        }

        if (d->vi->format->sampleType == stInteger) {
            if (d->vi->format->bitsPerSample == 8)
                TCanny<uint8_t>(src, dst, fa, stack, d, vsapi);
            else
                TCanny<uint16_t>(src, dst, fa, stack, d, vsapi);
        } else {
            TCanny<float>(src, dst, fa, stack, d, vsapi);
        }

        vsapi->freeFrame(src);
        for (int i = 0; i < 3; i++)
            vs_aligned_free(fa[i]);
        vs_aligned_free(stack.map);
        vs_aligned_free(stack.pos);
        return dst;
    }

    return nullptr;
}

static void VS_CC tcannyFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    TCannyData * d = static_cast<TCannyData *>(instanceData);
    vsapi->freeNode(d->node);
    vs_aligned_free(d->weights);
    delete d;
}

static void VS_CC tcannyCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    TCannyData d;
    int err;

    d.sigma = static_cast<float>(vsapi->propGetFloat(in, "sigma", 0, &err));
    if (err)
        d.sigma = 1.5f;
    d.t_h = static_cast<float>(vsapi->propGetFloat(in, "t_h", 0, &err));
    if (err)
        d.t_h = 8.f;
    d.t_l = static_cast<float>(vsapi->propGetFloat(in, "t_l", 0, &err));
    if (err)
        d.t_l = 1.f;
    d.nms = int64ToIntS(vsapi->propGetInt(in, "nms", 0, &err));
    if (err)
        d.nms = 3;
    d.mode = int64ToIntS(vsapi->propGetInt(in, "mode", 0, &err));
    d.op = int64ToIntS(vsapi->propGetInt(in, "op", 0, &err));
    if (err)
        d.op = 1;
    d.gmmax = static_cast<float>(vsapi->propGetFloat(in, "gmmax", 0, &err));
    if (err)
        d.gmmax = 50.f;

    if (d.sigma <= 0.f) {
        vsapi->setError(out, "TCanny: sigma must be greater than 0.0");
        return;
    }
    if (d.nms < 0 || d.nms > 3) {
        vsapi->setError(out, "TCanny: nms must be set to 0, 1, 2 or 3");
        return;
    }
    if (d.mode < -1 || d.mode > 3) {
        vsapi->setError(out, "TCanny: mode must be set to -1, 0, 1, 2 or 3");
        return;
    }
    if (d.op < 0 || d.op > 2) {
        vsapi->setError(out, "TCanny: op must be set to 0, 1 or 2");
        return;
    }
    if (d.gmmax < 1.f) {
        vsapi->setError(out, "TCanny: gmmax must be greater than or equal to 1.0");
        return;
    }

    d.node = vsapi->propGetNode(in, "clip", 0, nullptr);
    d.vi = vsapi->getVideoInfo(d.node);

    if (!isConstantFormat(d.vi) || (d.vi->format->sampleType == stInteger && d.vi->format->bitsPerSample > 16) ||
        (d.vi->format->sampleType == stFloat && d.vi->format->bitsPerSample != 32)) {
        vsapi->setError(out, "TCanny: only constant format 8-16 bits integer and 32 bits float input supported");
        vsapi->freeNode(d.node);
        return;
    }

    const int m = vsapi->propNumElements(in, "planes");

    for (int i = 0; i < 3; i++)
        d.process[i] = m <= 0;

    for (int i = 0; i < m; i++) {
        const int n = int64ToIntS(vsapi->propGetInt(in, "planes", i, nullptr));

        if (n < 0 || n >= d.vi->format->numPlanes) {
            vsapi->setError(out, "TCanny: plane index out of range");
            vsapi->freeNode(d.node);
            return;
        }

        if (d.process[n]) {
            vsapi->setError(out, "TCanny: plane specified twice");
            vsapi->freeNode(d.node);
            return;
        }

        d.process[n] = true;
    }

    if (d.vi->format->sampleType == stInteger) {
        const float scale = static_cast<float>(1 << (d.vi->format->bitsPerSample - 8));
        d.t_h *= scale;
        d.t_l *= scale;
        d.bins = 1 << d.vi->format->bitsPerSample;
        d.peak = d.bins - 1;
    } else {
        d.t_h /= 255.f;
        d.t_l /= 255.f;
        d.bins = 1;

        for (int plane = 0; plane < d.vi->format->numPlanes; plane++) {
            if (d.process[plane]) {
                if (plane == 0 || d.vi->format->colorFamily == cmRGB) {
                    d.lower[plane] = 0.f;
                    d.upper[plane] = 1.f;
                } else {
                    d.lower[plane] = -0.5f;
                    d.upper[plane] = 0.5f;
                }
            }
        }
    }

    d.weights = gaussianWeights(d.sigma, d.grad);
    if (!d.weights) {
        vsapi->setError(out, "TCanny: malloc failure (weights)");
        vsapi->freeNode(d.node);
        return;
    }

    d.magnitude = 255.f / d.gmmax;

    TCannyData * data = new TCannyData(d);

    vsapi->createFilter(in, out, "TCanny", tcannyInit, tcannyGetFrame, tcannyFree, fmParallel, 0, data, core);
}

//////////////////////////////////////////
// Init

VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.holywu.tcanny", "tcanny", "Build an edge map using canny edge detection", VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("TCanny", "clip:clip;sigma:float:opt;t_h:float:opt;t_l:float:opt;nms:int:opt;mode:int:opt;op:int:opt;gmmax:float:opt;planes:int[]:opt;", tcannyCreate, nullptr, plugin);
}
