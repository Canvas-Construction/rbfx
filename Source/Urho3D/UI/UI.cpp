//
// Copyright (c) 2008-2022 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include "../Core/Context.h"
#include "../Core/CoreEvents.h"
#include "../Core/Profiler.h"
#include "../Graphics/Graphics.h"
#include "../Graphics/GraphicsEvents.h"
#include "../Graphics/Shader.h"
#include "../Graphics/ShaderVariation.h"
#include "../Graphics/Texture2D.h"
#include "../Graphics/VertexBuffer.h"
#include "../Graphics/Octree.h"
#include "../Graphics/Viewport.h"
#include "../Graphics/Camera.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/Technique.h"
#include "../Scene/Scene.h"
#include "../Input/Input.h"
#include "../Input/InputEvents.h"
#include "../IO/Log.h"
#include "../Math/Matrix3x4.h"
#include "../Resource/ResourceCache.h"
#include "../UI/CheckBox.h"
#include "../UI/Cursor.h"
#include "../UI/DropDownList.h"
#include "../UI/FileSelector.h"
#include "../UI/Font.h"
#include "../UI/LineEdit.h"
#include "../UI/ListView.h"
#include "../UI/MessageBox.h"
#include "../UI/ProgressBar.h"
#include "../UI/ScrollBar.h"
#include "../UI/Slider.h"
#include "../UI/Sprite.h"
#include "../UI/Text.h"
#include "../UI/Text3D.h"
#include "../UI/TextRenderer3D.h"
#include "../UI/ToolTip.h"
#include "../UI/UI.h"
#include "../UI/UIEvents.h"
#include "../UI/Window.h"
#include "../UI/View3D.h"
#include "../UI/UIComponent.h"

#include <cassert>
#include <SDL.h>

#include "../DebugNew.h"

namespace Urho3D
{

static MouseButton MakeTouchIDMask(int id)
{
    return static_cast<MouseButton>(1u << static_cast<MouseButtonFlags::Integer>(id)); // NOLINT(misc-misplaced-widening-cast)
}

ea::string VAR_ORIGIN("Origin");
const ea::string VAR_ORIGINAL_PARENT("OriginalParent");
const ea::string VAR_ORIGINAL_CHILD_INDEX("OriginalChildIndex");
const ea::string VAR_PARENT_CHANGED("ParentChanged");

const float DEFAULT_DOUBLECLICK_INTERVAL = 0.5f;
const float DEFAULT_DRAGBEGIN_INTERVAL = 0.5f;
const float DEFAULT_TOOLTIP_DELAY = 0.5f;
const int DEFAULT_DRAGBEGIN_DISTANCE = 5;
const int DEFAULT_FONT_TEXTURE_MAX_SIZE = 2048;

UI::UI(Context* context) :
    Object(context),
    rootElement_(MakeShared<UIElement>(context)),
    rootModalElement_(MakeShared<UIElement>(context)),
    doubleClickInterval_(DEFAULT_DOUBLECLICK_INTERVAL),
    dragBeginInterval_(DEFAULT_DRAGBEGIN_INTERVAL),
    defaultToolTipDelay_(DEFAULT_TOOLTIP_DELAY),
    dragBeginDistance_(DEFAULT_DRAGBEGIN_DISTANCE),
    mouseButtons_(0),
    lastMouseButtons_(0),
    maxDoubleClickDist_(M_LARGE_VALUE),
    qualifiers_(0),
    maxFontTextureSize_(DEFAULT_FONT_TEXTURE_MAX_SIZE),
    initialized_(false),
    usingTouchInput_(false),
#ifdef _WIN32
    nonFocusedMouseWheel_(false),    // Default MS Windows behaviour
#else
    nonFocusedMouseWheel_(true),     // Default Mac OS X and Linux behaviour
#endif
    useSystemClipboard_(false),
#if defined(__ANDROID__) || defined(IOS) || defined(TVOS)
    useScreenKeyboard_(true),
#else
    useScreenKeyboard_(false),
#endif
    useMutableGlyphs_(false),
    forceAutoHint_(false),
    fontHintLevel_(FONT_HINT_LEVEL_NORMAL),
    fontSubpixelThreshold_(12),
    fontOversampling_(2),
    uiRendered_(false),
    nonModalBatchSize_(0),
    dragElementsCount_(0),
    dragConfirmedCount_(0),
    uiScale_(1.0f),
    customSize_(IntVector2::ZERO)
{
    rootElement_->SetTraversalMode(TM_DEPTH_FIRST);
    rootModalElement_->SetTraversalMode(TM_DEPTH_FIRST);
    Input* input = context_->GetSubsystem<Input>();
    URHO3D_ASSERT(input);
    SubscribeToEvent(E_SCREENMODE, URHO3D_HANDLER(UI, HandleScreenMode));
    SubscribeToEvent(input, E_MOUSEBUTTONDOWN, URHO3D_HANDLER(UI, HandleMouseButtonDown));
    SubscribeToEvent(input, E_MOUSEBUTTONUP, URHO3D_HANDLER(UI, HandleMouseButtonUp));
    SubscribeToEvent(input, E_MOUSEMOVE, URHO3D_HANDLER(UI, HandleMouseMove));
    SubscribeToEvent(input, E_MOUSEWHEEL, URHO3D_HANDLER(UI, HandleMouseWheel));
    SubscribeToEvent(input, E_TOUCHBEGIN, URHO3D_HANDLER(UI, HandleTouchBegin));
    SubscribeToEvent(input, E_TOUCHEND, URHO3D_HANDLER(UI, HandleTouchEnd));
    SubscribeToEvent(input, E_TOUCHMOVE, URHO3D_HANDLER(UI, HandleTouchMove));
    SubscribeToEvent(input, E_KEYDOWN, URHO3D_HANDLER(UI, HandleKeyDown));
    SubscribeToEvent(input, E_TEXTINPUT, URHO3D_HANDLER(UI, HandleTextInput));
    SubscribeToEvent(input, E_DROPFILE, URHO3D_HANDLER(UI, HandleDropFile));
    SubscribeToEvent(E_FOCUSED, URHO3D_HANDLER(UI, HandleFocused));

    // Try to initialize right now, but skip if screen mode is not yet set
    Initialize();
}

UI::~UI() = default;

void UI::SetCursor(Cursor* cursor)
{
    if (cursor_ == cursor)
        return;

    // Remove old cursor (if any) and set new
    if (cursor_)
    {
        rootElement_->RemoveChild(cursor_);
        cursor_.Reset();
    }
    if (cursor)
    {
        rootElement_->AddChild(cursor);
        cursor_ = cursor;

        IntVector2 pos = cursor_->GetPosition();
        const IntVector2& rootSize = rootElement_->GetSize();
        const IntVector2& rootPos = rootElement_->GetPosition();
        pos.x_ = Clamp(pos.x_, rootPos.x_, rootPos.x_ + rootSize.x_ - 1);
        pos.y_ = Clamp(pos.y_, rootPos.y_, rootPos.y_ + rootSize.y_ - 1);
        cursor_->SetPosition(pos);
    }
}

void UI::SetFocusElement(UIElement* element, bool byKey)
{
    using namespace FocusChanged;

    UIElement* originalElement = element;

    if (element)
    {
        // Return if already has focus
        if (focusElement_ == element)
            return;

        // Only allow child elements of the modal element to receive focus
        if (HasModalElement())
        {
            UIElement* topLevel = element->GetParent();
            while (topLevel && topLevel->GetParent() != rootElement_)
                topLevel = topLevel->GetParent();
            if (topLevel)   // If parented to non-modal root then ignore
                return;
        }

        // Search for an element in the hierarchy that can alter focus. If none found, exit
        element = GetFocusableElement(element);
        if (!element)
            return;
    }

    // Remove focus from the old element
    if (focusElement_)
    {
        UIElement* oldFocusElement = focusElement_;
        focusElement_.Reset();

        VariantMap& focusEventData = GetEventDataMap();
        focusEventData[Defocused::P_ELEMENT] = oldFocusElement;
        oldFocusElement->SendEvent(E_DEFOCUSED, focusEventData);
    }

    // Then set focus to the new
    if (element && element->GetFocusMode() >= FM_FOCUSABLE)
    {
        focusElement_ = element;

        VariantMap& focusEventData = GetEventDataMap();
        focusEventData[Focused::P_ELEMENT] = element;
        focusEventData[Focused::P_BYKEY] = byKey;
        element->SendEvent(E_FOCUSED, focusEventData);
    }

    VariantMap& eventData = GetEventDataMap();
    eventData[P_CLICKEDELEMENT] = originalElement;
    eventData[P_ELEMENT] = element;
    SendEvent(E_FOCUSCHANGED, eventData);
}

bool UI::SetModalElement(UIElement* modalElement, bool enable)
{
    if (!modalElement)
        return false;

    // Currently only allow modal window
    if (modalElement->GetType() != Window::GetTypeStatic())
        return false;

    assert(rootModalElement_);
    UIElement* currParent = modalElement->GetParent();
    if (enable)
    {
        // Make sure it is not already the child of the root modal element
        if (currParent == rootModalElement_)
            return false;

        // Adopt modal root as parent
        modalElement->SetVar(VAR_ORIGINAL_PARENT, currParent);
        modalElement->SetVar(VAR_ORIGINAL_CHILD_INDEX, currParent ? currParent->FindChild(modalElement) : M_MAX_UNSIGNED);
        modalElement->SetParent(rootModalElement_);

        // If it is a popup element, bring along its top-level parent
        auto* originElement = static_cast<UIElement*>(modalElement->GetVar(VAR_ORIGIN).GetPtr());
        if (originElement)
        {
            UIElement* element = originElement;
            while (element && element->GetParent() != rootElement_)
                element = element->GetParent();
            if (element)
            {
                originElement->SetVar(VAR_PARENT_CHANGED, element);
                UIElement* oriParent = element->GetParent();
                element->SetVar(VAR_ORIGINAL_PARENT, oriParent);
                element->SetVar(VAR_ORIGINAL_CHILD_INDEX, oriParent ? oriParent->FindChild(element) : M_MAX_UNSIGNED);
                element->SetParent(rootModalElement_);
            }
        }

        return true;
    }
    else
    {
        // Only the modal element can disable itself
        if (currParent != rootModalElement_)
            return false;

        // Revert back to original parent
        modalElement->SetParent(static_cast<UIElement*>(modalElement->GetVar(VAR_ORIGINAL_PARENT).GetPtr()),
            modalElement->GetVar(VAR_ORIGINAL_CHILD_INDEX).GetUInt());
        auto& vars = const_cast<StringVariantMap&>(modalElement->GetVars());
        vars.erase(VAR_ORIGINAL_PARENT);
        vars.erase(VAR_ORIGINAL_CHILD_INDEX);

        // If it is a popup element, revert back its top-level parent
        auto* originElement = static_cast<UIElement*>(modalElement->GetVar(VAR_ORIGIN).GetPtr());
        if (originElement)
        {
            auto* element = static_cast<UIElement*>(originElement->GetVar(VAR_PARENT_CHANGED).GetPtr());
            if (element)
            {
                const_cast<StringVariantMap&>(originElement->GetVars()).erase(VAR_PARENT_CHANGED);
                element->SetParent(static_cast<UIElement*>(element->GetVar(VAR_ORIGINAL_PARENT).GetPtr()),
                    element->GetVar(VAR_ORIGINAL_CHILD_INDEX).GetUInt());
                vars = const_cast<StringVariantMap&>(element->GetVars());
                vars.erase(VAR_ORIGINAL_PARENT);
                vars.erase(VAR_ORIGINAL_CHILD_INDEX);
            }
        }

        return true;
    }
}

void UI::Clear()
{
    rootElement_->RemoveAllChildren();
    rootModalElement_->RemoveAllChildren();
    if (cursor_)
        rootElement_->AddChild(cursor_);

    batches_.clear();
    vertexData_.clear();
    debugDrawBatches_.clear();
    debugVertexData_.clear();
}

void UI::Update(float timeStep)
{
    assert(rootElement_ && rootModalElement_);

    URHO3D_PROFILE("UpdateUI");

    // Expire hovers
    for (auto i = hoveredElements_.begin(); i !=
        hoveredElements_.end(); ++i)
        i->second = false;

    auto* input = GetSubsystem<Input>();
    bool mouseGrabbed = input->IsMouseGrabbed();

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    // Drag begin based on time
    if (dragElementsCount_ > 0 && !mouseGrabbed)
    {
        for (auto i = dragElements_.begin(); i !=
            dragElements_.end();)
        {
            WeakPtr<UIElement> dragElement = i->first;
            UI::DragData* dragData = i->second;

            if (!dragElement)
            {
                i = DragElementErase(i);
                continue;
            }

            if (!dragData->dragBeginPending)
            {
                ++i;
                continue;
            }

            if (dragData->dragBeginTimer.GetMSec(false) >= (unsigned)(dragBeginInterval_ * 1000))
            {
                dragData->dragBeginPending = false;
                IntVector2 beginSendPos = dragData->dragBeginSumPos / dragData->numDragButtons;
                dragConfirmedCount_++;
                if (!usingTouchInput_)
                    dragElement->OnDragBegin(dragElement->ScreenToElement(beginSendPos), beginSendPos, dragData->dragButtons,
                        qualifiers_, cursor_);
                else
                    dragElement->OnDragBegin(dragElement->ScreenToElement(beginSendPos), beginSendPos, dragData->dragButtons, QUAL_NONE, nullptr);

                SendDragOrHoverEvent(E_DRAGBEGIN, dragElement, beginSendPos, IntVector2::ZERO, dragData);
            }

            ++i;
        }
    }

    // Mouse hover
    if (!mouseGrabbed && !input->GetTouchEmulation())
    {
        if (!usingTouchInput_ && cursorVisible)
            ProcessHover(cursorPos, mouseButtons_, qualifiers_, cursor_);
    }

    // Touch hover
    unsigned numTouches = input->GetNumTouches();
    for (unsigned i = 0; i < numTouches; ++i)
    {
        TouchState* touch = input->GetTouch(i);
        IntVector2 touchPos = touch->position_;
        touchPos = ConvertSystemToUI(touchPos);
        ProcessHover(touchPos, MakeTouchIDMask(touch->touchID_), QUAL_NONE, nullptr);
    }

    // End hovers that expired without refreshing
    for (auto i = hoveredElements_.begin(); i !=
        hoveredElements_.end();)
    {
        if (i->first.Expired() || !i->second)
        {
            UIElement* element = i->first;
            if (element)
            {
                using namespace HoverEnd;

                VariantMap& eventData = GetEventDataMap();
                eventData[P_ELEMENT] = element;
                element->SendEvent(E_HOVEREND, eventData);
            }
            i = hoveredElements_.erase(i);
        }
        else
            ++i;
    }

    Update(timeStep, rootElement_);
    Update(timeStep, rootModalElement_);
}

void UI::RenderUpdate()
{
    assert(rootElement_ && rootModalElement_ && graphics_);

    URHO3D_PROFILE("GetUIBatches");

    uiRendered_ = false;

    // If the OS cursor is visible, do not render the UI's own cursor
    bool osCursorVisible = GetSubsystem<Input>()->IsMouseVisible();

    // Get rendering batches from the non-modal UI elements
    batches_.clear();
    vertexData_.clear();
    const IntVector2& rootSize = rootElement_->GetSize();
    const IntVector2& rootPos = rootElement_->GetPosition();
    // Note: the scissors operate on unscaled coordinates. Scissor scaling is only performed during render
    IntRect currentScissor = IntRect(rootPos.x_, rootPos.y_, rootPos.x_ + rootSize.x_, rootPos.y_ + rootSize.y_);
    if (rootElement_->IsVisible())
        GetBatches(batches_, vertexData_, rootElement_, currentScissor);

    // Save the batch size of the non-modal batches for later use
    nonModalBatchSize_ = batches_.size();

    // Get rendering batches from the modal UI elements
    GetBatches(batches_, vertexData_, rootModalElement_, currentScissor);

    // Get batches from the cursor (and its possible children) last to draw it on top of everything
    if (cursor_ && cursor_->IsVisible() && !osCursorVisible)
    {
        currentScissor = IntRect(0, 0, rootSize.x_, rootSize.y_);
        cursor_->GetBatches(batches_, vertexData_, currentScissor);
        GetBatches(batches_, vertexData_, cursor_, currentScissor);
    }

    // UIElement does not have anything to show. Insert dummy batch that will clear the texture.
    if (batches_.empty() && texture_ && clearColor_.a_ > 0)
    {
        UIBatch batch(rootElement_, BLEND_REPLACE, currentScissor, nullptr, &vertexData_);
        batch.SetColor(Color::BLACK);
        batch.AddQuad(currentScissor.left_, currentScissor.top_, currentScissor.right_, currentScissor.bottom_, 0, 0);
        batches_.push_back(batch);
    }
}

void UI::Render()
{
    URHO3D_PROFILE("RenderUI");

    // If the OS cursor is visible, apply its shape now if changed
    bool osCursorVisible = GetSubsystem<Input>()->IsMouseVisible();
    if (cursor_ && osCursorVisible)
        cursor_->ApplyOSCursorShape();

    SetVertexData(vertexBuffer_, vertexData_);
    SetVertexData(debugVertexBuffer_, debugVertexData_);

    // Render non-modal batches
    Render(vertexBuffer_, batches_, 0, nonModalBatchSize_);
    // Render debug draw
    Render(debugVertexBuffer_, debugDrawBatches_, 0, debugDrawBatches_.size());
    // Render modal batches
    Render(vertexBuffer_, batches_, nonModalBatchSize_, batches_.size());

    // Clear the debug draw batches and data
    debugDrawBatches_.clear();
    debugVertexData_.clear();

    uiRendered_ = true;
}

void UI::DebugDraw(UIElement* element)
{
    if (element)
    {
        UIElement* root = element->GetRoot();
        if (!root)
            root = element;
        const IntVector2& rootSize = root->GetSize();
        const IntVector2& rootPos = root->GetPosition();
        IntRect scissor(rootPos.x_, rootPos.y_, rootPos.x_ + rootSize.x_, rootPos.y_ + rootSize.y_);
        element->GetDebugDrawBatches(debugDrawBatches_, debugVertexData_, scissor);
    }
}

SharedPtr<UIElement> UI::LoadLayout(Deserializer& source, XMLFile* styleFile)
{
    SharedPtr<XMLFile> xml(MakeShared<XMLFile>(context_));
    if (!xml->Load(source))
        return SharedPtr<UIElement>();
    else
        return LoadLayout(xml, styleFile);
}

SharedPtr<UIElement> UI::LoadLayout(XMLFile* file, XMLFile* styleFile)
{
    URHO3D_PROFILE("LoadUILayout");

    SharedPtr<UIElement> root;

    if (!file)
    {
        URHO3D_LOGERROR("Null UI layout XML file");
        return root;
    }

    URHO3D_LOGDEBUG("Loading UI layout " + file->GetName());

    XMLElement rootElem = file->GetRoot("element");
    if (!rootElem)
    {
        URHO3D_LOGERROR("No root UI element in " + file->GetName());
        return root;
    }

    ea::string typeName = rootElem.GetAttribute("type");
    if (typeName.empty())
        typeName = "UIElement";

    root = DynamicCast<UIElement>(context_->CreateObject(typeName));
    if (!root)
    {
        URHO3D_LOGERROR("Could not create unknown UI element " + typeName);
        return root;
    }

    // Use default style file of the root element if it has one
    if (!styleFile)
        styleFile = rootElement_->GetDefaultStyle(false);
    // Set it as default for later use by children elements
    if (styleFile)
        root->SetDefaultStyle(styleFile);

    root->LoadXML(rootElem, styleFile);
    return root;
}

bool UI::SaveLayout(Serializer& dest, UIElement* element)
{
    URHO3D_PROFILE("SaveUILayout");

    return element && element->SaveXML(dest);
}

void UI::SetClipboardText(const ea::string& text)
{
    clipBoard_ = text;
    if (useSystemClipboard_)
        SDL_SetClipboardText(text.c_str());
}

void UI::SetDoubleClickInterval(float interval)
{
    doubleClickInterval_ = Max(interval, 0.0f);
}

void UI::SetMaxDoubleClickDistance(float distPixels)
{
    maxDoubleClickDist_ = distPixels;
}

void UI::SetDragBeginInterval(float interval)
{
    dragBeginInterval_ = Max(interval, 0.0f);
}

void UI::SetDragBeginDistance(int pixels)
{
    dragBeginDistance_ = Max(pixels, 0);
}

void UI::SetDefaultToolTipDelay(float delay)
{
    defaultToolTipDelay_ = Max(delay, 0.0f);
}

void UI::SetMaxFontTextureSize(int size)
{
    if (IsPowerOfTwo((unsigned)size) && size >= FONT_TEXTURE_MIN_SIZE)
    {
        if (size != maxFontTextureSize_)
        {
            maxFontTextureSize_ = size;
            ReleaseFontFaces();
        }
    }
}

void UI::SetNonFocusedMouseWheel(bool nonFocusedMouseWheel)
{
    nonFocusedMouseWheel_ = nonFocusedMouseWheel;
}

void UI::SetUseSystemClipboard(bool enable)
{
    useSystemClipboard_ = enable;
}

void UI::SetUseScreenKeyboard(bool enable)
{
    useScreenKeyboard_ = enable;
}

void UI::SetUseMutableGlyphs(bool enable)
{
    if (enable != useMutableGlyphs_)
    {
        useMutableGlyphs_ = enable;
        ReleaseFontFaces();
    }
}

void UI::SetForceAutoHint(bool enable)
{
    if (enable != forceAutoHint_)
    {
        forceAutoHint_ = enable;
        ReleaseFontFaces();
    }
}

void UI::SetFontHintLevel(FontHintLevel level)
{
    if (level != fontHintLevel_)
    {
        fontHintLevel_ = level;
        ReleaseFontFaces();
    }
}

void UI::SetFontSubpixelThreshold(float threshold)
{
    assert(threshold >= 0);
    if (threshold != fontSubpixelThreshold_)
    {
        fontSubpixelThreshold_ = threshold;
        ReleaseFontFaces();
    }
}

void UI::SetFontOversampling(int oversampling)
{
    assert(oversampling >= 1);
    oversampling = Clamp(oversampling, 1, 8);
    if (oversampling != fontOversampling_)
    {
        fontOversampling_ = oversampling;
        ReleaseFontFaces();
    }
}

void UI::SetScale(float scale)
{
    uiScale_ = Max(scale, M_EPSILON);
    ResizeRootElement();
}

void UI::SetWidth(float width)
{
    IntVector2 size = GetEffectiveRootElementSize(false);
    SetScale((float)size.x_ / width);
}

void UI::SetHeight(float height)
{
    IntVector2 size = GetEffectiveRootElementSize(false);
    SetScale((float)size.y_ / height);
}

void UI::SetCustomSize(const IntVector2& size)
{
    customSize_ = IntVector2(Max(0, size.x_), Max(0, size.y_));
    ResizeRootElement();
}

void UI::SetCustomSize(int width, int height)
{
    customSize_ = IntVector2(Max(0, width), Max(0, height));
    ResizeRootElement();
}

IntVector2 UI::GetUICursorPosition() const
{
    if (cursor_)
        return cursor_->GetPosition();

    return ConvertSystemToUI(GetSubsystem<Input>()->GetMousePosition());
}

IntVector2 UI::GetSystemCursorPosition() const
{
    if (cursor_)
        return ConvertUIToSystem(cursor_->GetPosition());

    return GetSubsystem<Input>()->GetMousePosition();
}

UIElement* UI::GetElementAt(const IntVector2& position, bool enabledOnly)
{
    UIElement* result = nullptr;

    if (HasModalElement())
        result = GetElementAt(rootModalElement_, position, enabledOnly);

    if (!result)
        result = GetElementAt(rootElement_, position, enabledOnly);

    return result;
}

UIElement* UI::GetElementAt(UIElement* root, const IntVector2& position, bool enabledOnly)
{
    IntVector2 positionCopy(position);
    const IntVector2& rootSize = root->GetSize();
    const IntVector2& rootPos = root->GetPosition();

    // If position is out of bounds of root element return null.
    if (position.x_ < rootPos.x_ || position.x_ > rootPos.x_ + rootSize.x_)
        return nullptr;

    if (position.y_ < rootPos.y_ || position.y_ > rootPos.y_ + rootSize.y_)
        return nullptr;

    // If UI is smaller than the screen, wrap if necessary
    if (rootSize.x_ > 0 && rootSize.y_ > 0)
    {
        if (positionCopy.x_ >= rootPos.x_ + rootSize.x_)
            positionCopy.x_ = rootPos.x_ + ((positionCopy.x_ - rootPos.x_) % rootSize.x_);
        if (positionCopy.y_ >= rootPos.y_ + rootSize.y_)
            positionCopy.y_ = rootPos.y_ + ((positionCopy.y_ - rootPos.y_) % rootSize.y_);
    }

    UIElement* result = nullptr;
    GetElementAt(result, root, positionCopy, enabledOnly);
    return result;
}

UIElement* UI::GetElementAt(int x, int y, bool enabledOnly)
{
    return GetElementAt(IntVector2(x, y), enabledOnly);
}

IntVector2 UI::ConvertSystemToUI(const IntVector2& systemPos) const
{
    return VectorFloorToInt(systemPos.ToVector2() / GetScale());
}

IntVector2 UI::ConvertUIToSystem(const IntVector2& uiPos) const
{
    return VectorFloorToInt(uiPos.ToVector2() * GetScale());
}

UIElement* UI::GetFrontElement() const
{
    const ea::vector<SharedPtr<UIElement> >& rootChildren = rootElement_->GetChildren();
    int maxPriority = M_MIN_INT;
    UIElement* front = nullptr;

    for (unsigned i = 0; i < rootChildren.size(); ++i)
    {
        // Do not take into account input-disabled elements, hidden elements or those that are always in the front
        if (!rootChildren[i]->IsEnabled() || !rootChildren[i]->IsVisible() || !rootChildren[i]->GetBringToBack())
            continue;

        int priority = rootChildren[i]->GetPriority();
        if (priority > maxPriority)
        {
            maxPriority = priority;
            front = rootChildren[i];
        }
    }

    return front;
}

const ea::vector<UIElement*>& UI::GetDragElements()
{
    // Do not return the element until drag begin event has actually been posted
    if (!dragElementsConfirmed_.empty())
        return dragElementsConfirmed_;

    for (auto i = dragElements_.begin(); i !=
        dragElements_.end();)
    {
        WeakPtr<UIElement> dragElement = i->first;
        UI::DragData* dragData = i->second;

        if (!dragElement)
        {
            i = DragElementErase(i);
            continue;
        }

        if (!dragData->dragBeginPending)
            dragElementsConfirmed_.push_back(dragElement);

        ++i;
    }

    return dragElementsConfirmed_;
}

UIElement* UI::GetDragElement(unsigned index)
{
    GetDragElements();
    if (index >= dragElementsConfirmed_.size())
        return nullptr;

    return dragElementsConfirmed_[index];
}

const ea::string& UI::GetClipboardText() const
{
    if (useSystemClipboard_)
    {
        char* text = SDL_GetClipboardText();
        clipBoard_ = ea::string(text);
        if (text)
            SDL_free(text);
    }

    return clipBoard_;
}

bool UI::HasModalElement() const
{
    return rootModalElement_->GetNumChildren() > 0;
}

void UI::Initialize()
{
    auto* graphics = GetSubsystem<Graphics>();

    if (!graphics || !graphics->IsInitialized())
        return;

    URHO3D_PROFILE("InitUI");

    graphics_ = graphics;
    renderer_ = GetSubsystem<Renderer>();

    // Set initial root element size
    ResizeRootElement();

    vertexBuffer_ = MakeShared<VertexBuffer>(context_);
    debugVertexBuffer_ = MakeShared<VertexBuffer>(context_);

    batchStateCache_ = MakeShared<DefaultUIBatchStateCache>(context_);

    ea::string baseDefines = "VERTEXCOLOR ";
    if (graphics->GetCaps().constantBuffersSupported_)
        baseDefines += "URHO3D_USE_CBUFFERS ";
    const ea::string alphaMapDefines = baseDefines + "ALPHAMAP ";
    const ea::string diffMapDefines = baseDefines + "DIFFMAP ";
    const ea::string diffMapAlphaMaskDefines = diffMapDefines + "ALPHAMASK ";

    noTextureMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", baseDefines, baseDefines);
    alphaMapMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", alphaMapDefines, alphaMapDefines);
    diffMapMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", diffMapDefines, diffMapDefines);
    diffMapAlphaMaskMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", diffMapDefines, diffMapAlphaMaskDefines);

    initialized_ = true;

    SubscribeToEvent(E_BEGINFRAME, URHO3D_HANDLER(UI, HandleBeginFrame));
    SubscribeToEvent(E_POSTUPDATE, URHO3D_HANDLER(UI, HandlePostUpdate));
    SubscribeToEvent(E_RENDERUPDATE, URHO3D_HANDLER(UI, HandleRenderUpdate));
}

void UI::Update(float timeStep, UIElement* element)
{
    // Keep a weak pointer to the element in case it destroys itself on update
    WeakPtr<UIElement> elementWeak(element);

    element->Update(timeStep);
    if (elementWeak.Expired())
        return;

    const ea::vector<SharedPtr<UIElement> >& children = element->GetChildren();
    // Update of an element may modify its child vector. Use just index-based iteration to be safe
    for (unsigned i = 0; i < children.size(); ++i)
        Update(timeStep, children[i]);
}

void UI::SetVertexData(VertexBuffer* dest, const ea::vector<float>& vertexData)
{
    if (vertexData.empty())
        return;

    // Update quad geometry into the vertex buffer
    // Resize the vertex buffer first if too small or much too large
    unsigned numVertices = vertexData.size() / UI_VERTEX_SIZE;
    if (dest->GetVertexCount() < numVertices || dest->GetVertexCount() > numVertices * 2)
        dest->SetSize(numVertices, MASK_POSITION | MASK_COLOR | MASK_TEXCOORD1, true);

    dest->SetData(&vertexData[0]);
}

Material* UI::GetBatchMaterial(const UIBatch& batch) const
{
    if (batch.customMaterial_)
        return batch.customMaterial_;

    if (!batch.texture_)
        return noTextureMaterial_;
    else if (batch.texture_->GetFormat() == Graphics::GetAlphaFormat())
        return alphaMapMaterial_;
    else if (batch.blendMode_ != BLEND_ALPHA && batch.blendMode_ != BLEND_ADDALPHA && batch.blendMode_ != BLEND_PREMULALPHA)
        return diffMapAlphaMaskMaterial_;
    else
        return diffMapMaterial_;
}

void UI::Render(VertexBuffer* buffer, const ea::vector<UIBatch>& batches, unsigned batchStart, unsigned batchEnd)
{
    // Engine does not render when window is closed or device is lost
    assert(graphics_ && graphics_->IsInitialized() && !graphics_->IsDeviceLost());

    if (batches.empty())
        return;

    DrawCommandQueue* drawQueue = renderer_->GetDefaultDrawQueue();

    unsigned alphaFormat = Graphics::GetAlphaFormat();
    RenderSurface* surface = graphics_->GetRenderTarget(0);
    const bool isSurfaceSRGB = RenderSurface::GetSRGB(graphics_, surface);
    IntVector2 viewSize = graphics_->GetViewport().Size();
    Vector2 invScreenSize(1.0f / (float)viewSize.x_, 1.0f / (float)viewSize.y_);
    Vector2 scale(2.0f * invScreenSize.x_, -2.0f * invScreenSize.y_);
    Vector2 offset(-1.0f, 1.0f);

    if (surface)
    {
#ifdef URHO3D_OPENGL
        // On OpenGL, flip the projection if rendering to a texture so that the texture can be addressed in the
        // same way as a render texture produced on Direct3D.
        offset.y_ = -offset.y_;
        scale.y_ = -scale.y_;
#endif
    }

    Matrix4 projection(Matrix4::IDENTITY);
    projection.m00_ = scale.x_ * uiScale_;
    projection.m03_ = offset.x_;
    projection.m11_ = scale.y_ * uiScale_;
    projection.m13_ = offset.y_;
    projection.m22_ = 1.0f;
    projection.m23_ = 0.0f;
    projection.m33_ = 1.0f;

    drawQueue->Reset();

    const float elapsedTime = GetSubsystem<Time>()->GetElapsedTime();
    UIBatchStateCreateContext batchStateCreateContext{ vertexBuffer_, nullptr };
    for (unsigned i = batchStart; i < batchEnd; ++i)
    {
        const UIBatch& batch = batches[i];
        if (batch.vertexStart_ == batch.vertexEnd_)
            continue;

        Material* material = GetBatchMaterial(batch);
        const UIBatchStateKey key{ isSurfaceSRGB, material, material->GetDefaultPass(), batch.blendMode_ };
        PipelineState* pipelineState = batchStateCache_->GetOrCreatePipelineState(key, batchStateCreateContext);
        if (!pipelineState || !pipelineState->IsValid())
            continue;

        drawQueue->SetPipelineState(pipelineState);

        if (drawQueue->BeginShaderParameterGroup(SP_FRAME))
        {
            drawQueue->AddShaderParameter(VSP_ELAPSEDTIME, elapsedTime);
            drawQueue->AddShaderParameter(PSP_ELAPSEDTIME, elapsedTime);
            drawQueue->CommitShaderParameterGroup(SP_FRAME);
        }

        if (drawQueue->BeginShaderParameterGroup(SP_OBJECT))
        {
            drawQueue->AddShaderParameter(VSP_MODEL, Matrix3x4::IDENTITY);
            drawQueue->CommitShaderParameterGroup(SP_OBJECT);
        }

        if (drawQueue->BeginShaderParameterGroup(SP_CAMERA))
        {
            drawQueue->AddShaderParameter(VSP_VIEWPROJ, projection);
            drawQueue->CommitShaderParameterGroup(SP_CAMERA);
        }

        if (drawQueue->BeginShaderParameterGroup(SP_MATERIAL))
        {
            if (!batch.customMaterial_)
                drawQueue->AddShaderParameter(PSP_MATDIFFCOLOR, Color(1.0f, 1.0f, 1.0f, 1.0f));
            else
            {
                for (const auto& param : batch.customMaterial_->GetShaderParameters())
                    drawQueue->AddShaderParameter(param.first, param.second.value_);
            }
            drawQueue->CommitShaderParameterGroup(SP_MATERIAL);
        }

        drawQueue->SetBuffers({ { vertexBuffer_ }, nullptr, nullptr });

        IntRect scissor = batch.scissor_;
        scissor.left_ = (int)(scissor.left_ * uiScale_);
        scissor.top_ = (int)(scissor.top_ * uiScale_);
        scissor.right_ = (int)(scissor.right_ * uiScale_);
        scissor.bottom_ = (int)(scissor.bottom_ * uiScale_);

        // Flip scissor vertically if using OpenGL texture rendering
#ifdef URHO3D_OPENGL
        if (surface)
        {
            int top = scissor.top_;
            int bottom = scissor.bottom_;
            scissor.top_ = viewSize.y_ - bottom;
            scissor.bottom_ = viewSize.y_ - top;
        }
#endif
        drawQueue->SetScissorRect(scissor);

        if (!batch.customMaterial_)
            drawQueue->AddShaderResource(TU_DIFFUSE, batch.texture_);
        else
        {
            for (const auto& texture : batch.customMaterial_->GetTextures())
                drawQueue->AddShaderResource(texture.first, texture.second);
        }
        drawQueue->CommitShaderResources();

        drawQueue->Draw(batch.vertexStart_ / UI_VERTEX_SIZE, (batch.vertexEnd_ - batch.vertexStart_) / UI_VERTEX_SIZE);
    }

    drawQueue->Execute();
}

void UI::GetBatches(ea::vector<UIBatch>& batches, ea::vector<float>& vertexData, UIElement* element, IntRect currentScissor)
{
    // Set clipping scissor for child elements. No need to draw if zero size
    element->AdjustScissor(currentScissor);
    if (currentScissor.left_ == currentScissor.right_ || currentScissor.top_ == currentScissor.bottom_)
        return;

    element->SortChildren();
    const ea::vector<SharedPtr<UIElement> >& children = element->GetChildren();
    if (children.empty())
        return;

    // For non-root elements draw all children of same priority before recursing into their children: assumption is that they have
    // same renderstate
    auto i = children.begin();
    if (element->GetTraversalMode() == TM_BREADTH_FIRST)
    {
        auto j = i;
        while (i != children.end())
        {
            int currentPriority = (*i)->GetPriority();
            while (j != children.end() && (*j)->GetPriority() == currentPriority)
            {
                if ((*j)->IsWithinScissor(currentScissor) && (*j) != cursor_)
                    (*j)->GetBatches(batches, vertexData, currentScissor);
                ++j;
            }
            // Now recurse into the children
            while (i != j)
            {
                if ((*i)->IsVisible() && (*i) != cursor_)
                    GetBatches(batches, vertexData, *i, currentScissor);
                ++i;
            }
        }
    }
    // On the root level draw each element and its children immediately after to avoid artifacts
    else
    {
        while (i != children.end())
        {
            if ((*i) != cursor_)
            {
                if ((*i)->IsWithinScissor(currentScissor))
                    (*i)->GetBatches(batches, vertexData, currentScissor);
                if ((*i)->IsVisible())
                    GetBatches(batches, vertexData, *i, currentScissor);
            }
            ++i;
        }
    }
}

void UI::GetElementAt(UIElement*& result, UIElement* current, const IntVector2& position, bool enabledOnly)
{
    if (!current)
        return;

    current->SortChildren();
    const ea::vector<SharedPtr<UIElement> >& children = current->GetChildren();
    LayoutMode parentLayoutMode = current->GetLayoutMode();

    for (unsigned i = 0; i < children.size(); ++i)
    {
        UIElement* element = children[i];
        bool hasChildren = element->GetNumChildren() > 0;

        if (element != cursor_ && element->IsVisible())
        {
            if (element->IsInside(position, true))
            {
                // Store the current result, then recurse into its children. Because children
                // are sorted from lowest to highest priority, the topmost match should remain
                if (element->IsEnabled() || !enabledOnly)
                    result = element;

                if (hasChildren)
                    GetElementAt(result, element, position, enabledOnly);
                // Layout optimization: if the element has no children, can break out after the first match
                else if (parentLayoutMode != LM_FREE)
                    break;
            }
            else
            {
                if (hasChildren)
                {
                    if (element->IsInsideCombined(position, true))
                        GetElementAt(result, element, position, enabledOnly);
                }
                // Layout optimization: if position is much beyond the visible screen, check how many elements we can skip,
                // or if we already passed all visible elements
                else if (parentLayoutMode != LM_FREE)
                {
                    if (!i)
                    {
                        int screenPos = (parentLayoutMode == LM_HORIZONTAL) ? element->GetScreenPosition().x_ :
                            element->GetScreenPosition().y_;
                        int layoutMaxSize = current->GetLayoutElementMaxSize();
                        int spacing = current->GetLayoutSpacing();

                        if (screenPos < 0 && layoutMaxSize > 0)
                        {
                            auto toSkip = (unsigned)(-screenPos / (layoutMaxSize + spacing));
                            if (toSkip > 0)
                                i += (toSkip - 1);
                        }
                    }
                    // Note: we cannot check for the up / left limits of positioning, since the element may be off the visible
                    // screen but some of its layouted children will yet be visible. In down & right directions we can terminate
                    // the loop, since all further children will be further down or right.
                    else if (parentLayoutMode == LM_HORIZONTAL)
                    {
                        if (element->GetScreenPosition().x_ >= rootElement_->GetPosition().x_ + rootElement_->GetSize().x_)
                            break;
                    }
                    else if (parentLayoutMode == LM_VERTICAL)
                    {
                        if (element->GetScreenPosition().y_ >= rootElement_->GetPosition().y_ + rootElement_->GetSize().y_)
                            break;
                    }
                }
            }
        }
    }
}

UIElement* UI::GetFocusableElement(UIElement* element)
{
    while (element)
    {
        if (element->GetFocusMode() != FM_NOTFOCUSABLE)
            break;
        element = element->GetParent();
    }
    return element;
}

UIElement* UI::GetWheelHandlerElement(UIElement* element)
{
    while (element)
    {
        if (element->IsWheelHandler())
            break;
        element = element->GetParent();
    }
    return element;
}

void UI::GetCursorPositionAndVisible(IntVector2& pos, bool& visible)
{
    // Prefer software cursor then OS-specific cursor
    if (cursor_ && cursor_->IsVisible())
    {
        pos = cursor_->GetPosition();
        visible = true;
    }
    else if (GetSubsystem<Input>()->GetMouseMode() == MM_RELATIVE)
        visible = true;
    else
    {
        auto* input = GetSubsystem<Input>();
        visible = input->IsMouseVisible();

        if (!visible && cursor_)
        {
            pos = cursor_->GetPosition();
        }
        else
        {
            pos = rootElement_->ScreenToElement(input->GetMousePosition());
            pos = ConvertSystemToUI(pos);
        }
    }
}

void UI::SetCursorShape(CursorShape shape)
{
    if (cursor_)
        cursor_->SetShape(shape);
}

void UI::ReleaseFontFaces()
{
    URHO3D_LOGDEBUG("Reloading font faces");

    ea::vector<Font*> fonts;
    GetSubsystem<ResourceCache>()->GetResources<Font>(fonts);

    for (unsigned i = 0; i < fonts.size(); ++i)
        fonts[i]->ReleaseFaces();
}

void UI::ProcessHover(const IntVector2& windowCursorPos, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor)
{
    IntVector2 cursorPos = windowCursorPos;
    WeakPtr<UIElement> element(GetElementAt(windowCursorPos, true));

    for (auto i = dragElements_.begin(); i !=
        dragElements_.end();)
    {
        WeakPtr<UIElement> dragElement = i->first;
        UI::DragData* dragData = i->second;

        if (!dragElement)
        {
            i = DragElementErase(i);
            continue;
        }

        bool dragSource = dragElement && (dragElement->GetDragDropMode() & DD_SOURCE);
        bool dragTarget = element && (element->GetDragDropMode() & DD_TARGET);
        bool dragDropTest = dragSource && dragTarget && element != dragElement;
        // If drag start event has not been posted yet, do not do drag handling here
        if (dragData->dragBeginPending)
            dragSource = dragTarget = dragDropTest = false;

        // Hover effect
        // If a drag is going on, transmit hover only to the element being dragged, unless it's a drop target
        if (element && element->IsEnabled())
        {
            if (dragElement == element || dragDropTest)
            {
                element->OnHover(element->ScreenToElement(cursorPos), cursorPos, buttons, qualifiers, cursor);

                // Begin hover event
                if (!hoveredElements_.contains(element))
                {
                    SendDragOrHoverEvent(E_HOVERBEGIN, element, cursorPos, IntVector2::ZERO, nullptr);
                    // Exit if element is destroyed by the event handling
                    if (!element)
                        return;
                }
                hoveredElements_[element] = true;
            }
        }

        // Drag and drop test
        if (dragDropTest)
        {
            bool accept = element->OnDragDropTest(dragElement);
            if (accept)
            {
                using namespace DragDropTest;

                VariantMap& eventData = GetEventDataMap();
                eventData[P_SOURCE] = dragElement;
                eventData[P_TARGET] = element;
                eventData[P_ACCEPT] = accept;
                SendEvent(E_DRAGDROPTEST, eventData);
                accept = eventData[P_ACCEPT].GetBool();
            }

            if (cursor)
                cursor->SetShape(accept ? CS_ACCEPTDROP : CS_REJECTDROP);
        }
        else if (dragSource && cursor)
            cursor->SetShape(dragElement == element ? CS_ACCEPTDROP : CS_REJECTDROP);

        ++i;
    }

    // Hover effect
    // If no drag is going on, transmit hover event.
    if (element && element->IsEnabled())
    {
        if (dragElementsCount_ == 0)
        {
            element->OnHover(element->ScreenToElement(cursorPos), cursorPos, buttons, qualifiers, cursor);

            // Begin hover event
            if (!hoveredElements_.contains(element))
            {
                SendDragOrHoverEvent(E_HOVERBEGIN, element, cursorPos, IntVector2::ZERO, nullptr);
                // Exit if element is destroyed by the event handling
                if (!element)
                    return;
            }
            hoveredElements_[element] = true;
        }
    }
}

void UI::ProcessClickBegin(const IntVector2& windowCursorPos, MouseButton button, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor, bool cursorVisible)
{
    if (cursorVisible)
    {
        IntVector2 cursorPos = windowCursorPos;
        WeakPtr<UIElement> element(GetElementAt(windowCursorPos, true));

        bool newButton;
        if (usingTouchInput_)
            newButton = (buttons & button) == MOUSEB_NONE;
        else
            newButton = true;
        buttons |= button;

        if (element)
            SetFocusElement(element);

        // Focus change events may destroy the element, check again.
        if (element)
        {
            // Handle focusing & bringing to front
            element->BringToFront();

            // Handle click
            element->OnClickBegin(element->ScreenToElement(cursorPos), cursorPos, button, buttons, qualifiers, cursor);
            SendClickEvent(E_UIMOUSECLICK, nullptr, element, cursorPos, button, buttons, qualifiers);

            // Fire double click event if element matches and is in time and is within max distance from the original click
            if (doubleClickElement_ && element == doubleClickElement_ &&
                (clickTimer_.GetMSec(true) < (unsigned)(doubleClickInterval_ * 1000)) && lastMouseButtons_ == buttons && (windowCursorPos - doubleClickFirstPos_).Length() < maxDoubleClickDist_)
            {
                element->OnDoubleClick(element->ScreenToElement(cursorPos), cursorPos, button, buttons, qualifiers, cursor);
                doubleClickElement_.Reset();
                SendDoubleClickEvent(nullptr, element, doubleClickFirstPos_, cursorPos, button, buttons, qualifiers);
            }
            else
            {
                doubleClickElement_ = element;
                doubleClickFirstPos_ = windowCursorPos;
                clickTimer_.Reset();
            }

            // Handle start of drag. Click handling may have caused destruction of the element, so check the pointer again
            bool dragElementsContain = dragElements_.contains(element);
            if (element && !dragElementsContain)
            {
                auto* dragData = new DragData();
                dragElements_[element] = dragData;
                dragData->dragBeginPending = true;
                dragData->sumPos = cursorPos;
                dragData->dragBeginSumPos = cursorPos;
                dragData->dragBeginTimer.Reset();
                dragData->dragButtons = button;
                dragData->numDragButtons = CountSetBits((unsigned)dragData->dragButtons);
                dragElementsCount_++;

                dragElementsContain = dragElements_.contains(element);
            }
            else if (element && dragElementsContain && newButton)
            {
                DragData* dragData = dragElements_[element];
                dragData->sumPos += cursorPos;
                dragData->dragBeginSumPos += cursorPos;
                dragData->dragButtons |= button;
                dragData->numDragButtons = CountSetBits((unsigned)dragData->dragButtons);
            }
        }
        else
        {
            // If clicked over no element, or a disabled element, lose focus (but not if there is a modal element)
            if (!HasModalElement())
                SetFocusElement(nullptr);
            SendClickEvent(E_UIMOUSECLICK, nullptr, element, cursorPos, button, buttons, qualifiers);

            if (clickTimer_.GetMSec(true) < (unsigned)(doubleClickInterval_ * 1000) && lastMouseButtons_ == buttons && (windowCursorPos - doubleClickFirstPos_).Length() < maxDoubleClickDist_)
                SendDoubleClickEvent(nullptr, element, doubleClickFirstPos_, cursorPos, button, buttons, qualifiers);
        }

        lastMouseButtons_ = buttons;
    }
}

void UI::ProcessClickEnd(const IntVector2& windowCursorPos, MouseButton button, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor, bool cursorVisible)
{
    WeakPtr<UIElement> element;
    IntVector2 cursorPos = windowCursorPos;
    if (cursorVisible)
        element = GetElementAt(cursorPos, true);

    // Handle end of drag
    for (auto i = dragElements_.begin(); i !=
        dragElements_.end();)
    {
        WeakPtr<UIElement> dragElement = i->first;
        UI::DragData* dragData = i->second;

        if (!dragElement || !cursorVisible)
        {
            i = DragElementErase(i);
            continue;
        }

        if (dragData->dragButtons & button)
        {
            // Handle end of click
            if (element)
                element->OnClickEnd(element->ScreenToElement(cursorPos), cursorPos, button, buttons, qualifiers, cursor,
                    dragElement);

            SendClickEvent(E_UIMOUSECLICKEND, dragElement, element, cursorPos, button, buttons, qualifiers);

            if (dragElement && dragElement->IsEnabled() && dragElement->IsVisible() && !dragData->dragBeginPending)
            {
                dragElement->OnDragEnd(dragElement->ScreenToElement(cursorPos), cursorPos, dragData->dragButtons, buttons,
                    cursor);
                SendDragOrHoverEvent(E_DRAGEND, dragElement, cursorPos, IntVector2::ZERO, dragData);

                bool dragSource = dragElement && (dragElement->GetDragDropMode() & DD_SOURCE);
                if (dragSource)
                {
                    bool dragTarget = element && (element->GetDragDropMode() & DD_TARGET);
                    bool dragDropFinish = dragSource && dragTarget && element != dragElement;

                    if (dragDropFinish)
                    {
                        bool accept = element->OnDragDropFinish(dragElement);

                        // OnDragDropFinish() may have caused destruction of the elements, so check the pointers again
                        if (accept && dragElement && element)
                        {
                            using namespace DragDropFinish;

                            VariantMap& eventData = GetEventDataMap();
                            eventData[P_SOURCE] = dragElement;
                            eventData[P_TARGET] = element;
                            eventData[P_ACCEPT] = accept;
                            SendEvent(E_DRAGDROPFINISH, eventData);
                        }
                    }
                }
            }

            i = DragElementErase(i);
        }
        else
            ++i;
    }
}

void UI::ProcessMove(const IntVector2& windowCursorPos, const IntVector2& cursorDeltaPos, MouseButtonFlags buttons, QualifierFlags qualifiers, Cursor* cursor,
    bool cursorVisible)
{
    if (cursorVisible && dragElementsCount_ > 0 && buttons)
    {
        IntVector2 cursorPos = windowCursorPos;

        auto* input = GetSubsystem<Input>();
        bool mouseGrabbed = input->IsMouseGrabbed();
        for (auto i = dragElements_.begin(); i !=
            dragElements_.end();)
        {
            WeakPtr<UIElement> dragElement = i->first;
            UI::DragData* dragData = i->second;

            if (!dragElement)
            {
                i = DragElementErase(i);
                continue;
            }

            if (!(dragData->dragButtons & buttons))
            {
                ++i;
                continue;
            }

            // Calculate the position that we should send for this drag event.
            IntVector2 sendPos;
            if (usingTouchInput_)
            {
                dragData->sumPos += cursorDeltaPos;
                sendPos.x_ = dragData->sumPos.x_ / dragData->numDragButtons;
                sendPos.y_ = dragData->sumPos.y_ / dragData->numDragButtons;
            }
            else
            {
                dragData->sumPos = cursorPos;
                sendPos = cursorPos;
            }

            if (dragElement->IsEnabled() && dragElement->IsVisible())
            {
                // Signal drag begin if distance threshold was exceeded

                if (dragData->dragBeginPending && !mouseGrabbed)
                {
                    IntVector2 beginSendPos;
                    beginSendPos.x_ = dragData->dragBeginSumPos.x_ / dragData->numDragButtons;
                    beginSendPos.y_ = dragData->dragBeginSumPos.y_ / dragData->numDragButtons;

                    IntVector2 offset = cursorPos - beginSendPos;
                    if (Abs(offset.x_) >= dragBeginDistance_ || Abs(offset.y_) >= dragBeginDistance_)
                    {
                        dragData->dragBeginPending = false;
                        dragConfirmedCount_++;
                        dragElement->OnDragBegin(dragElement->ScreenToElement(beginSendPos), beginSendPos, buttons, qualifiers,
                            cursor);
                        SendDragOrHoverEvent(E_DRAGBEGIN, dragElement, beginSendPos, IntVector2::ZERO, dragData);
                    }
                }

                if (!dragData->dragBeginPending)
                {
                    dragElement->OnDragMove(dragElement->ScreenToElement(sendPos), sendPos, cursorDeltaPos, buttons, qualifiers,
                        cursor);
                    SendDragOrHoverEvent(E_DRAGMOVE, dragElement, sendPos, cursorDeltaPos, dragData);
                }
            }
            else
            {
                dragElement->OnDragEnd(dragElement->ScreenToElement(sendPos), sendPos, dragData->dragButtons, buttons, cursor);
                SendDragOrHoverEvent(E_DRAGEND, dragElement, sendPos, IntVector2::ZERO, dragData);
                dragElement.Reset();
            }

            ++i;
        }
    }
}

void UI::SendDragOrHoverEvent(StringHash eventType, UIElement* element, const IntVector2& screenPos, const IntVector2& deltaPos,
    UI::DragData* dragData)
{
    if (!element)
        return;

    IntVector2 relativePos = element->ScreenToElement(screenPos);

    using namespace DragMove;

    VariantMap& eventData = GetEventDataMap();
    eventData[P_ELEMENT] = element;
    eventData[P_X] = screenPos.x_;
    eventData[P_Y] = screenPos.y_;
    eventData[P_ELEMENTX] = relativePos.x_;
    eventData[P_ELEMENTY] = relativePos.y_;

    if (eventType == E_DRAGMOVE)
    {
        eventData[P_DX] = deltaPos.x_;
        eventData[P_DY] = deltaPos.y_;
    }

    if (dragData)
    {
        eventData[P_BUTTONS] = (unsigned)dragData->dragButtons;
        eventData[P_NUMBUTTONS] = dragData->numDragButtons;
    }

    element->SendEvent(eventType, eventData);
}

void UI::SendClickEvent(StringHash eventType, UIElement* beginElement, UIElement* endElement, const IntVector2& pos, MouseButton button,
    MouseButtonFlags buttons, QualifierFlags qualifiers)
{
    VariantMap& eventData = GetEventDataMap();
    eventData[UIMouseClick::P_ELEMENT] = endElement;
    eventData[UIMouseClick::P_X] = pos.x_;
    eventData[UIMouseClick::P_Y] = pos.y_;
    eventData[UIMouseClick::P_BUTTON] = button;
    eventData[UIMouseClick::P_BUTTONS] = (unsigned)buttons;
    eventData[UIMouseClick::P_QUALIFIERS] = (unsigned)qualifiers;

    // For click end events, send also the element the click began on
    if (eventType == E_UIMOUSECLICKEND)
        eventData[UIMouseClickEnd::P_BEGINELEMENT] = beginElement;

    if (endElement)
    {
        // Send also element version of the event
        if (eventType == E_UIMOUSECLICK)
            endElement->SendEvent(E_CLICK, eventData);
        else if (eventType == E_UIMOUSECLICKEND)
            endElement->SendEvent(E_CLICKEND, eventData);
    }

    // Send the global event from the UI subsystem last
    SendEvent(eventType, eventData);
}

void UI::SendDoubleClickEvent(UIElement* beginElement, UIElement* endElement, const IntVector2& firstPos, const IntVector2& secondPos, MouseButton button,
    MouseButtonFlags buttons, QualifierFlags qualifiers)
{
    VariantMap& eventData = GetEventDataMap();
    eventData[UIMouseDoubleClick::P_ELEMENT] = endElement;
    eventData[UIMouseDoubleClick::P_X] = secondPos.x_;
    eventData[UIMouseDoubleClick::P_Y] = secondPos.y_;
    eventData[UIMouseDoubleClick::P_XBEGIN] = firstPos.x_;
    eventData[UIMouseDoubleClick::P_YBEGIN] = firstPos.y_;
    eventData[UIMouseDoubleClick::P_BUTTON] = button;
    eventData[UIMouseDoubleClick::P_BUTTONS] = (unsigned)buttons;
    eventData[UIMouseDoubleClick::P_QUALIFIERS] = (unsigned)qualifiers;


    if (endElement)
    {
        // Send also element version of the event
        endElement->SendEvent(E_DOUBLECLICK, eventData);
    }

    // Send the global event from the UI subsystem last
    SendEvent(E_UIMOUSEDOUBLECLICK, eventData);
}


void UI::HandleScreenMode(StringHash eventType, VariantMap& eventData)
{
    using namespace ScreenMode;

    if (!initialized_)
        Initialize();
    else
        ResizeRootElement();
}

void UI::HandleMouseButtonDown(StringHash eventType, VariantMap& eventData)
{
    using namespace MouseButtonDown;

    mouseButtons_ = MouseButtonFlags(eventData[P_BUTTONS].GetUInt());
    qualifiers_ = QualifierFlags(eventData[P_QUALIFIERS].GetUInt());
    usingTouchInput_ = false;

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    // Handle drag cancelling
    ProcessDragCancel();

    auto* input = GetSubsystem<Input>();

    if (!input->IsMouseGrabbed())
        ProcessClickBegin(cursorPos, MouseButton(eventData[P_BUTTON].GetUInt()), mouseButtons_, qualifiers_, cursor_, cursorVisible);
}

void UI::HandleMouseButtonUp(StringHash eventType, VariantMap& eventData)
{
    using namespace MouseButtonUp;

    mouseButtons_ = MouseButtonFlags(eventData[P_BUTTONS].GetUInt());
    qualifiers_ = QualifierFlags(eventData[P_QUALIFIERS].GetUInt());

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    ProcessClickEnd(cursorPos, (MouseButton)eventData[P_BUTTON].GetUInt(), mouseButtons_, qualifiers_, cursor_, cursorVisible);
}

void UI::HandleMouseMove(StringHash eventType, VariantMap& eventData)
{
    using namespace MouseMove;

    mouseButtons_ = MouseButtonFlags(eventData[P_BUTTONS].GetUInt());
    qualifiers_ = QualifierFlags(eventData[P_QUALIFIERS].GetUInt());
    usingTouchInput_ = false;

    auto* input = GetSubsystem<Input>();
    const IntVector2& rootSize = rootElement_->GetSize();
    const IntVector2& rootPos = rootElement_->GetPosition();

    const IntVector2 mouseDeltaPos{ eventData[P_DX].GetInt(), eventData[P_DY].GetInt() };
    const IntVector2 mousePos{ eventData[P_X].GetInt(), eventData[P_Y].GetInt() };

    if (cursor_)
    {
        if (!input->IsMouseVisible())
        {
            if (!input->IsMouseLocked())
                cursor_->SetPosition(rootElement_->ScreenToElement(ConvertSystemToUI(mousePos)));
            else if (cursor_->IsVisible())
            {
                // Relative mouse motion: move cursor only when visible
                IntVector2 pos = cursor_->GetPosition();
                pos += ConvertSystemToUI(mouseDeltaPos);
                pos.x_ = Clamp(pos.x_, rootPos.x_, rootPos.x_ + rootSize.x_ - 1);
                pos.y_ = Clamp(pos.y_, rootPos.y_, rootPos.y_ + rootSize.y_ - 1);
                cursor_->SetPosition(pos);
            }
        }
        else
        {
            // Absolute mouse motion: move always
            cursor_->SetPosition(rootElement_->ScreenToElement(ConvertSystemToUI(mousePos)));
        }
    }

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    ProcessMove(cursorPos, mouseDeltaPos, mouseButtons_, qualifiers_, cursor_, cursorVisible);
}

void UI::HandleMouseWheel(StringHash eventType, VariantMap& eventData)
{
    auto* input = GetSubsystem<Input>();
    if (input->IsMouseGrabbed())
        return;

    using namespace MouseWheel;

    mouseButtons_ = MouseButtonFlags(eventData[P_BUTTONS].GetInt());
    qualifiers_ = QualifierFlags(eventData[P_QUALIFIERS].GetInt());
    int delta = eventData[P_WHEEL].GetInt();
    usingTouchInput_ = false;

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    if (!nonFocusedMouseWheel_ && focusElement_)
    {
        if (UIElement* element = GetWheelHandlerElement(focusElement_))
            element->OnWheel(delta, mouseButtons_, qualifiers_);
    }
    else
    {
        // If no element has actual focus or in non-focused mode, get the element at cursor
        if (cursorVisible)
        {
            UIElement* element = GetElementAt(cursorPos);

            // Going up the hierarchy chain to find element that could handle mouse wheel
            element = GetWheelHandlerElement(element);

            if (element && (nonFocusedMouseWheel_ || element->GetFocusMode() >= FM_FOCUSABLE))
                element->OnWheel(delta, mouseButtons_, qualifiers_);
        }
    }
}

void UI::HandleTouchBegin(StringHash eventType, VariantMap& eventData)
{
    auto* input = GetSubsystem<Input>();
    if (input->IsMouseGrabbed())
        return;

    using namespace TouchBegin;

    IntVector2 pos(eventData[P_X].GetInt(), eventData[P_Y].GetInt());
    pos = ConvertSystemToUI(pos);
    usingTouchInput_ = true;

    const MouseButton touchId = MakeTouchIDMask(eventData[P_TOUCHID].GetInt());
    WeakPtr<UIElement> element(GetElementAt(pos));

    if (element)
    {
        ProcessClickBegin(pos, touchId, touchDragElements_[element], QUAL_NONE, nullptr, true);
        touchDragElements_[element] |= touchId;
    }
    else
        ProcessClickBegin(pos, touchId, touchId, QUAL_NONE, nullptr, true);
}

void UI::HandleTouchEnd(StringHash eventType, VariantMap& eventData)
{
    using namespace TouchEnd;

    IntVector2 pos(eventData[P_X].GetInt(), eventData[P_Y].GetInt());
    pos = ConvertSystemToUI(pos);

    // Get the touch index
    const MouseButton touchId = MakeTouchIDMask(eventData[P_TOUCHID].GetInt());

    // Transmit hover end to the position where the finger was lifted
    WeakPtr<UIElement> element(GetElementAt(pos));

    // Clear any drag events that were using the touch id
    for (auto i = touchDragElements_.begin(); i != touchDragElements_.end();)
    {
        const MouseButtonFlags touches = i->second;
        if (touches & touchId)
            i = touchDragElements_.erase(i);
        else
            ++i;
    }

    if (element && element->IsEnabled())
        element->OnHover(element->ScreenToElement(pos), pos, MOUSEB_NONE, QUAL_NONE, nullptr);

    ProcessClickEnd(pos, touchId, MOUSEB_NONE, QUAL_NONE, nullptr, true);
}

void UI::HandleTouchMove(StringHash eventType, VariantMap& eventData)
{
    using namespace TouchMove;

    IntVector2 pos(eventData[P_X].GetInt(), eventData[P_Y].GetInt());
    IntVector2 deltaPos(eventData[P_DX].GetInt(), eventData[P_DY].GetInt());
    pos = ConvertSystemToUI(pos);
    deltaPos = ConvertSystemToUI(deltaPos);
    usingTouchInput_ = true;

    const MouseButton touchId = MakeTouchIDMask(eventData[P_TOUCHID].GetInt());

    ProcessMove(pos, deltaPos, touchId, QUAL_NONE, nullptr, true);
}

void UI::HandleKeyDown(StringHash eventType, VariantMap& eventData)
{
    using namespace KeyDown;

    mouseButtons_ = MouseButtonFlags(eventData[P_BUTTONS].GetUInt());
    qualifiers_ = QualifierFlags(eventData[P_QUALIFIERS].GetUInt());
    auto key = (Key)eventData[P_KEY].GetUInt();

    // Cancel UI dragging
    if (key == KEY_ESCAPE && dragElementsCount_ > 0)
    {
        ProcessDragCancel();

        return;
    }

    // Dismiss modal element if any when ESC key is pressed
    if (key == KEY_ESCAPE && HasModalElement())
    {
        UIElement* element = rootModalElement_->GetChild(rootModalElement_->GetNumChildren() - 1);
        if (element->GetVars().contains(VAR_ORIGIN))
            // If it is a popup, dismiss by defocusing it
            SetFocusElement(nullptr);
        else
        {
            // If it is a modal window, by resetting its modal flag
            auto* window = dynamic_cast<Window*>(element);
            if (window && window->GetModalAutoDismiss())
                window->SetModal(false);
        }

        return;
    }

    UIElement* element = focusElement_;
    if (element)
    {
        // Switch focus between focusable elements in the same top level window
        if (key == KEY_TAB)
        {
            UIElement* topLevel = element->GetParent();
            while (topLevel && topLevel->GetParent() != rootElement_ && topLevel->GetParent() != rootModalElement_)
                topLevel = topLevel->GetParent();
            if (topLevel)
            {
                topLevel->GetChildren(tempElements_, true);
                for (auto i = tempElements_.begin(); i != tempElements_.end();)
                {
                    if ((*i)->GetFocusMode() < FM_FOCUSABLE)
                        i = tempElements_.erase(i);
                    else
                        ++i;
                }
                for (unsigned i = 0; i < tempElements_.size(); ++i)
                {
                    if (tempElements_[i] == element)
                    {
                        int dir = (qualifiers_ & QUAL_SHIFT) ? -1 : 1;
                        unsigned nextIndex = (tempElements_.size() + i + dir) % tempElements_.size();
                        UIElement* next = tempElements_[nextIndex];
                        SetFocusElement(next, true);
                        return;
                    }
                }
            }
        }
        // Defocus the element
        else if (key == KEY_ESCAPE && element->GetFocusMode() == FM_FOCUSABLE_DEFOCUSABLE)
            element->SetFocus(false);
        // If none of the special keys, pass the key to the focused element
        else
            element->OnKey(key, mouseButtons_, qualifiers_);
    }
}

void UI::HandleTextInput(StringHash eventType, VariantMap& eventData)
{
    using namespace TextInput;

    UIElement* element = focusElement_;
    if (element)
        element->OnTextInput(eventData[P_TEXT].GetString());
}

void UI::HandleBeginFrame(StringHash eventType, VariantMap& eventData)
{
    // If have a cursor, and a drag is not going on, reset the cursor shape. Application logic that wants to apply
    // custom shapes can do it after this, but needs to do it each frame
    if (cursor_ && dragElementsCount_ == 0)
        cursor_->SetShape(CS_NORMAL);
}

void UI::HandlePostUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace PostUpdate;

    Update(eventData[P_TIMESTEP].GetFloat());
}

void UI::HandleRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    RenderUpdate();
}

void UI::HandleDropFile(StringHash eventType, VariantMap& eventData)
{
    auto* input = GetSubsystem<Input>();

    // Sending the UI variant of the event only makes sense if the OS cursor is visible (not locked to window center)
    if (input->IsMouseVisible())
    {
        IntVector2 screenPos = input->GetMousePosition();
        screenPos = ConvertSystemToUI(screenPos);

        UIElement* element = GetElementAt(screenPos);

        using namespace UIDropFile;

        VariantMap uiEventData;
        uiEventData[P_FILENAME] = eventData[P_FILENAME];
        uiEventData[P_X] = screenPos.x_;
        uiEventData[P_Y] = screenPos.y_;
        uiEventData[P_ELEMENT] = element;

        if (element)
        {
            IntVector2 relativePos = element->ScreenToElement(screenPos);
            uiEventData[P_ELEMENTX] = relativePos.x_;
            uiEventData[P_ELEMENTY] = relativePos.y_;
        }

        SendEvent(E_UIDROPFILE, uiEventData);
    }
}

void UI::HandleFocused(StringHash eventType, VariantMap& eventData)
{
    using namespace Focused;
    if (UIElement* focusedElement = static_cast<UIElement*>(eventData[P_ELEMENT].GetPtr()))
    {
        if (focusElement_ != focusedElement)
            SetFocusElement(nullptr);
    }
}

void UI::HandleEndAllViewsRender(StringHash eventType, VariantMap& eventData)
{
    if (texture_)
    {
        if (RenderSurface* surface = texture_->GetRenderSurface())
        {
            graphics_->ResetRenderTargets();
            graphics_->SetDepthStencil(surface->GetLinkedDepthStencil());
            graphics_->SetRenderTarget(0, surface);
            graphics_->SetViewport(IntRect(0, 0, surface->GetWidth(), surface->GetHeight()));
            if (clearColor_.a_ > 0)
                graphics_->Clear(CLEAR_COLOR, clearColor_);
            Render();
        }
    }
}

ea::unordered_map<WeakPtr<UIElement>, UI::DragData*>::iterator UI::DragElementErase(ea::unordered_map<WeakPtr<UIElement>, DragData*>::iterator i)
{
    // If running the engine frame in response to an event (re-entering UI frame logic) the dragElements_ may already be empty
    if (dragElements_.empty())
        return dragElements_.end();

    dragElementsConfirmed_.clear();

    DragData* dragData = i->second;

    if (!dragData->dragBeginPending)
        --dragConfirmedCount_;
    i = dragElements_.erase(i);
    --dragElementsCount_;

    delete dragData;
    return i;
}

void UI::ProcessDragCancel()
{
    // How to tell difference between drag cancel and new selection on multi-touch?
    if (usingTouchInput_)
        return;

    IntVector2 cursorPos;
    bool cursorVisible;
    GetCursorPositionAndVisible(cursorPos, cursorVisible);

    for (auto i = dragElements_.begin(); i !=
        dragElements_.end();)
    {
        WeakPtr<UIElement> dragElement = i->first;
        UI::DragData* dragData = i->second;

        if (dragElement && dragElement->IsEnabled() && dragElement->IsVisible() && !dragData->dragBeginPending)
        {
            dragElement->OnDragCancel(dragElement->ScreenToElement(cursorPos), cursorPos, dragData->dragButtons, mouseButtons_,
                cursor_);
            SendDragOrHoverEvent(E_DRAGCANCEL, dragElement, cursorPos, IntVector2::ZERO, dragData);
            i = DragElementErase(i);
        }
        else
            ++i;
    }
}

IntVector2 UI::SumTouchPositions(UI::DragData* dragData, const IntVector2& oldSendPos)
{
    IntVector2 sendPos = oldSendPos;
    if (usingTouchInput_)
    {
        MouseButtonFlags buttons = dragData->dragButtons;
        dragData->sumPos = IntVector2::ZERO;
        auto* input = GetSubsystem<Input>();
        for (unsigned i = 0; (1u << i) <= (unsigned)buttons; i++)
        {
            auto mouseButton = static_cast<MouseButton>(1u << i); // NOLINT(misc-misplaced-widening-cast)
            if (buttons & mouseButton)
            {
                TouchState* ts = input->GetTouch((unsigned)i);
                if (!ts)
                    break;
                IntVector2 pos = ts->position_;
                pos = ConvertSystemToUI(pos);
                dragData->sumPos += pos;
            }
        }
        sendPos.x_ = dragData->sumPos.x_ / dragData->numDragButtons;
        sendPos.y_ = dragData->sumPos.y_ / dragData->numDragButtons;
    }
    return sendPos;
}

void UI::ResizeRootElement()
{
    IntVector2 effectiveSize = GetEffectiveRootElementSize();
    rootElement_->SetSize(effectiveSize);
    rootModalElement_->SetSize(effectiveSize);

    if (texture_)
    {
        if (texture_->GetWidth() != effectiveSize.x_ || texture_->GetHeight() != effectiveSize.y_)
        {
            unsigned format = texture_->GetFormat();
            if (format == 0)
                format = Graphics::GetRGBAFormat();
            if (texture_->SetSize(effectiveSize.x_, effectiveSize.y_, format, TEXTURE_RENDERTARGET,
                                  texture_->GetMultiSample(), texture_->GetAutoResolve()))
                texture_->GetRenderSurface()->SetUpdateMode(SURFACE_MANUALUPDATE);
            else
                URHO3D_LOGERROR("Resizing of UI render target texture failed.");
        }
    }
}

IntVector2 UI::GetSize() const
{
    // Use a fake size in headless mode
    IntVector2 size = graphics_ ? IntVector2(graphics_->GetWidth(), graphics_->GetHeight()) : IntVector2(1024, 768);
    size.x_ = RoundToInt(static_cast<float>(size.x_) / uiScale_);
    size.y_ = RoundToInt(static_cast<float>(size.y_) / uiScale_);
    return size;
}


IntVector2 UI::GetEffectiveRootElementSize(bool applyScale) const
{
    // Use a fake size in headless mode
    IntVector2 size = graphics_ ? IntVector2(graphics_->GetWidth(), graphics_->GetHeight()) : IntVector2(1024, 768);
    if (customSize_.x_ > 0 && customSize_.y_ > 0)
        size = customSize_;

    if (applyScale)
    {
        size.x_ = RoundToInt(static_cast<float>(size.x_) / uiScale_);
        size.y_ = RoundToInt(static_cast<float>(size.y_) / uiScale_);
    }

    return size;
}

void UI::SetRenderTarget(Texture2D* texture, Color clearColor)
{
    clearColor_ = clearColor;
    texture_ = texture;
    if (texture == nullptr)
        UnsubscribeFromEvent(E_ENDALLVIEWSRENDER);
    else
    {
        SubscribeToEvent(E_ENDALLVIEWSRENDER, URHO3D_HANDLER(UI, HandleEndAllViewsRender));
        ResizeRootElement();
    }
}

void UI::SetRoot(UIElement* root)
{
    rootElement_ = root;
    customSize_ = root->GetSize();
    ResizeRootElement();
}

void UI::SetRootModalElement(UIElement* rootModal)
{
    rootModalElement_ = rootModal;
    ResizeRootElement();
}

void RegisterUILibrary(Context* context)
{
    Font::RegisterObject(context);

    UIElement::RegisterObject(context);
    UISelectable::RegisterObject(context);
    BorderImage::RegisterObject(context);
    Sprite::RegisterObject(context);
    Button::RegisterObject(context);
    CheckBox::RegisterObject(context);
    Cursor::RegisterObject(context);
    Text::RegisterObject(context);
    Text3D::RegisterObject(context);
    TextRenderer3D::RegisterObject(context);
    Window::RegisterObject(context);
    View3D::RegisterObject(context);
    LineEdit::RegisterObject(context);
    Slider::RegisterObject(context);
    ScrollBar::RegisterObject(context);
    ScrollView::RegisterObject(context);
    ListView::RegisterObject(context);
    Menu::RegisterObject(context);
    DropDownList::RegisterObject(context);
    FileSelector::RegisterObject(context);
    MessageBox::RegisterObject(context);
    ProgressBar::RegisterObject(context);
    ToolTip::RegisterObject(context);
    UIComponent::RegisterObject(context);
}

}
