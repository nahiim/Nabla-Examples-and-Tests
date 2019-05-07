/*

MIT License

Copyright (c) 2019 InnerPiece Technology Co., Ltd.
https://innerpiece.io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#ifndef _IRR_BRDF_EXPLORER_INCLUDED_
#define _IRR_BRDF_EXPLORER_INCLUDED_

namespace irr
{
namespace video
{
class IVideoDriver;
}

namespace ext
{
namespace cegui
{
class GUIManager;
}
}

class BRDF {
    public:
        BRDF(video::IVideoDriver* driver);
        ~BRDF();

        void renderGUI();

    private:
        video::IVideoDriver* Driver = nullptr;
        ext::cegui::GUIManager* GUI = nullptr;
};

} // namespace irr

#endif // _IRR_BRDF_EXPLORER_INCLUDED_
