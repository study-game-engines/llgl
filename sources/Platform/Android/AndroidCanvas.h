/*
 * AndroidCanvas.h
 * 
 * This file is part of the "LLGL" project (Copyright (c) 2015-2019 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#ifndef LLGL_ANDROID_CANVAS_H
#define LLGL_ANDROID_CANVAS_H


#include <LLGL/Canvas.h>
#include <android/native_activity.h>


namespace LLGL
{


class AndroidCanvas : public Canvas
{

    public:

        AndroidCanvas(const CanvasDescriptor& desc);
        ~AndroidCanvas();

        bool GetNativeHandle(void* nativeHandle, std::size_t nativeHandleSize) const override;

        Extent2D GetContentSize() const override;

        void SetTitle(const UTF8String& title) override;
        UTF8String GetTitle() const override;

        void ResetPixelFormat() override;

    private:

        void OnProcessEvents() override;

    private:

        CanvasDescriptor    desc_;
        ANativeWindow*      window_         = nullptr;
        Extent2D            contentSize_;

};


} // /namespace LLGL


#endif



// ================================================================================
