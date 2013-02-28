//
// Copyright (c) 2002-2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// VertexDataManager.h: Defines the VertexDataManager, a class that
// runs the Buffer translation process.

#include "libGLESv2/renderer/VertexDataManager.h"

#include "common/debug.h"

#include "libGLESv2/renderer/Renderer9.h"
#include "libGLESv2/Buffer.h"
#include "libGLESv2/ProgramBinary.h"
#include "libGLESv2/main.h"

#include "libGLESv2/renderer/vertexconversion.h"
#include "libGLESv2/renderer/IndexDataManager.h"

#include <limits>

namespace
{
    enum { INITIAL_STREAM_BUFFER_SIZE = 1024*1024 };
    // This has to be at least 4k or else it fails on ATI cards.
    enum { CONSTANT_VERTEX_BUFFER_SIZE = 4096 };
}

namespace rx
{

static int elementsInBuffer(const gl::VertexAttribute &attribute, int size)
{
    int stride = attribute.stride();
    return (size - attribute.mOffset % stride + (stride - attribute.typeSize())) / stride;
}

VertexDataManager::VertexDataManager(Renderer *renderer) : mRenderer(renderer)
{
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        mCurrentValue[i][0] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i][1] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i][2] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValue[i][3] = std::numeric_limits<float>::quiet_NaN();
        mCurrentValueBuffer[i] = NULL;
        mCurrentValueOffsets[i] = 0;
    }

    mStreamingBuffer = new StreamingVertexBufferInterface(renderer, INITIAL_STREAM_BUFFER_SIZE);

    if (!mStreamingBuffer)
    {
        ERR("Failed to allocate the streaming vertex buffer.");
    }
}

VertexDataManager::~VertexDataManager()
{
    delete mStreamingBuffer;

    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        delete mCurrentValueBuffer[i];
    }
}

GLenum VertexDataManager::prepareVertexData(const gl::VertexAttribute attribs[], gl::ProgramBinary *programBinary, GLint start, GLsizei count, TranslatedAttribute *translated, GLsizei instances)
{
    if (!mStreamingBuffer)
    {
        return GL_OUT_OF_MEMORY;
    }

    for (int attributeIndex = 0; attributeIndex < gl::MAX_VERTEX_ATTRIBS; attributeIndex++)
    {
        translated[attributeIndex].active = (programBinary->getSemanticIndex(attributeIndex) != -1);
    }

    // Invalidate static buffers that don't contain matching attributes
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active && attribs[i].mArrayEnabled)
        {
            gl::Buffer *buffer = attribs[i].mBoundBuffer.get();
            StaticVertexBufferInterface *staticBuffer = buffer ? buffer->getStaticVertexBuffer() : NULL;

            if (staticBuffer)
            {
                if (staticBuffer->getBufferSize() == 0)
                {
                    int totalCount = elementsInBuffer(attribs[i], buffer->size());
                    staticBuffer->reserveVertexSpace(attribs[i], totalCount, 0);
                }
                else if (staticBuffer->lookupAttribute(attribs[i]) == -1)
                {
                    mStreamingBuffer->reserveVertexSpace(attribs[i], count, instances);
                    buffer->invalidateStaticData();
                }
            }
            else
            {
                mStreamingBuffer->reserveVertexSpace(attribs[i], count, instances);
            }
        }
    }

    // Perform the vertex data translations
    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active)
        {
            if (attribs[i].mArrayEnabled)
            {
                gl::Buffer *buffer = attribs[i].mBoundBuffer.get();

                if (!buffer && attribs[i].mPointer == NULL)
                {
                    // This is an application error that would normally result in a crash, but we catch it and return an error
                    ERR("An enabled vertex array has no buffer and no pointer.");
                    return GL_INVALID_OPERATION;
                }

                StaticVertexBufferInterface *staticBuffer = buffer ? buffer->getStaticVertexBuffer() : NULL;
                VertexBufferInterface *vertexBuffer = staticBuffer ? staticBuffer : static_cast<VertexBufferInterface*>(mStreamingBuffer);

                std::size_t streamOffset = -1;
                unsigned int outputElementSize = 0;

                if (staticBuffer)
                {
                    streamOffset = staticBuffer->lookupAttribute(attribs[i]);
                    outputElementSize = staticBuffer->getVertexBuffer()->getSpaceRequired(attribs[i], 1, 0);

                    if (streamOffset == -1)
                    {
                        // Convert the entire buffer
                        int totalCount = elementsInBuffer(attribs[i], buffer->size());
                        int startIndex = attribs[i].mOffset / attribs[i].stride();

                        streamOffset = staticBuffer->storeVertexAttributes(attribs[i], -startIndex, totalCount, 0);
                    }

                    if (streamOffset != -1)
                    {
                        streamOffset += (attribs[i].mOffset / attribs[i].stride()) * outputElementSize;

                        if (instances == 0 || attribs[i].mDivisor == 0)
                        {
                            streamOffset += start * outputElementSize;
                        }
                    }
                }
                else
                {
                    outputElementSize = mStreamingBuffer->getVertexBuffer()->getSpaceRequired(attribs[i], 1, 0);
                    streamOffset = mStreamingBuffer->storeVertexAttributes(attribs[i], start, count, instances);
                }

                if (streamOffset == -1)
                {
                    return GL_OUT_OF_MEMORY;
                }

                translated[i].vertexBuffer = vertexBuffer->getVertexBuffer();
                translated[i].serial = vertexBuffer->getSerial();
                translated[i].divisor = attribs[i].mDivisor;

                translated[i].attribute = &attribs[i];
                translated[i].stride = outputElementSize;
                translated[i].offset = streamOffset;
            }
            else
            {
                if (!mCurrentValueBuffer[i])
                {
                    mCurrentValueBuffer[i] = new StreamingVertexBufferInterface(mRenderer, CONSTANT_VERTEX_BUFFER_SIZE);
                }

                StreamingVertexBufferInterface *buffer = mCurrentValueBuffer[i];

                if (mCurrentValue[i][0] != attribs[i].mCurrentValue[0] ||
                    mCurrentValue[i][1] != attribs[i].mCurrentValue[1] ||
                    mCurrentValue[i][2] != attribs[i].mCurrentValue[2] ||
                    mCurrentValue[i][3] != attribs[i].mCurrentValue[3])
                {
                    unsigned int requiredSpace = sizeof(float) * 4;
                    buffer->reserveRawDataSpace(requiredSpace);
                    int streamOffset = buffer->storeRawData(attribs[i].mCurrentValue, requiredSpace);
                    if (streamOffset == -1)
                    {
                        return GL_OUT_OF_MEMORY;
                    }

                    mCurrentValueOffsets[i] = streamOffset;
                }

                translated[i].vertexBuffer = mCurrentValueBuffer[i]->getVertexBuffer();
                translated[i].serial = mCurrentValueBuffer[i]->getSerial();
                translated[i].divisor = 0;

                translated[i].attribute = &attribs[i];
                translated[i].stride = 0;
                translated[i].offset = mCurrentValueOffsets[i];
            }
        }
    }

    for (int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (translated[i].active && attribs[i].mArrayEnabled)
        {
            gl::Buffer *buffer = attribs[i].mBoundBuffer.get();

            if (buffer)
            {
                buffer->promoteStaticUsage(count * attribs[i].typeSize());
            }
        }
    }

    return GL_NO_ERROR;
}

}