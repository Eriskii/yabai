extern struct event_loop g_event_loop;
extern volatile bool __pending_gesture;
extern volatile uint64_t __last_gesture_time;

static inline uint8_t mouse_mod_from_cgflags(uint32_t cgflags)
{
    uint8_t flags = 0;

    if ((cgflags & kCGEventFlagMaskAlternate)   == kCGEventFlagMaskAlternate)   flags |= MOUSE_MOD_ALT;
    if ((cgflags & kCGEventFlagMaskShift)       == kCGEventFlagMaskShift)       flags |= MOUSE_MOD_SHIFT;
    if ((cgflags & kCGEventFlagMaskCommand)     == kCGEventFlagMaskCommand)     flags |= MOUSE_MOD_CMD;
    if ((cgflags & kCGEventFlagMaskControl)     == kCGEventFlagMaskControl)     flags |= MOUSE_MOD_CTRL;
    if ((cgflags & kCGEventFlagMaskSecondaryFn) == kCGEventFlagMaskSecondaryFn) flags |= MOUSE_MOD_FN;

    return flags;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static MOUSE_HANDLER(mouse_handler)
{
    struct mouse_state *mouse_state = context;

    switch (type) {
    case kCGEventTapDisabledByTimeout:
    case kCGEventTapDisabledByUserInput: {
        CFMachPortRef handle = __atomic_load_n(&mouse_state->handle, __ATOMIC_RELAXED);
        if (handle) CGEventTapEnable(handle, true);
    } break;
    case kCGEventLeftMouseDown:
    case kCGEventRightMouseDown: {
        uint8_t mod = mouse_mod_from_cgflags(CGEventGetFlags(event));
        event_loop_post(&g_event_loop, MOUSE_DOWN, (void *) CFRetain(event), mod);

        if (mod == mouse_state->modifier) {
            mouse_state->consume_mouse_click = true;
            mouse_state->consumed_event = (CGEventRef) CFRetain(event);
            return NULL;
        }
    } break;
    case kCGEventLeftMouseUp:
    case kCGEventRightMouseUp: {
        event_loop_post(&g_event_loop, MOUSE_UP, (void *) CFRetain(event), 0);

        if (mouse_state->consume_mouse_click) {
            if (!mouse_state->drag_detected) {
                CGEventTapPostEvent(proxy, mouse_state->consumed_event);
                CGEventTapPostEvent(proxy, event);
            }

            mouse_state->drag_detected = false;
            mouse_state->consume_mouse_click = false;
            CFRelease(mouse_state->consumed_event);
            return NULL;
        }
    } break;
    case kCGEventLeftMouseDragged:
    case kCGEventRightMouseDragged: {
        mouse_state->drag_detected = true;
        event_loop_post(&g_event_loop, MOUSE_DRAGGED, (void *) CFRetain(event), 0);
    } break;
    case kCGEventMouseMoved: {
        uint8_t mod = mouse_mod_from_cgflags(CGEventGetFlags(event));
        if (mod == mouse_state->modifier) return event;

        event_loop_post(&g_event_loop, MOUSE_MOVED, (void *) CFRetain(event), mod);
    } break;
    case /* kCGSEventDockControl */ 30: {
        int type = CGEventGetIntegerValueField(event, /* kCGEventGestureHIDType */ 110);
        if (type == /* kIOHIDEventTypeDockSwipe */ 23) {
            int motion = CGEventGetIntegerValueField(event, /* kCGEventGestureSwipeMotion */ 123);
            if (motion == /* kCGGestureMotionHorizontal */ 1) {
                int phase = CGEventGetIntegerValueField(event, /* kCGEventGesturePhase */ 132);
                if (phase == /* kCGSGesturePhaseBegan */ 1) {
                    __atomic_store_n(&__pending_gesture, true, __ATOMIC_RELEASE);
                } else if (phase == /* kCGSGesturePhaseEnded */ 4 || phase == /* kCGSGesturePhaseCancelled */ 8) {
                    __atomic_store_n(&__pending_gesture, false, __ATOMIC_RELEASE);
                    __atomic_store_n(&__last_gesture_time, read_os_timer(), __ATOMIC_RELEASE);
                }
            }
        }
    } break;
    }

    return event;
}
#pragma clang diagnostic pop

void mouse_window_info_populate(struct mouse_state *ms, struct mouse_window_info *info)
{
    CGRect frame = ms->window->frame;

    info->dx = frame.origin.x    - ms->window_frame.origin.x;
    info->dy = frame.origin.y    - ms->window_frame.origin.y;
    info->dw = frame.size.width  - ms->window_frame.size.width;
    info->dh = frame.size.height - ms->window_frame.size.height;

    info->changed_x = info->dx != 0.0f;
    info->changed_y = info->dy != 0.0f;
    info->changed_w = info->dw != 0.0f;
    info->changed_h = info->dh != 0.0f;

    info->changed_position = info->changed_x || info->changed_y;
    info->changed_size     = info->changed_w || info->changed_h;
}

enum mouse_drop_action mouse_determine_drop_action(struct mouse_state *ms, struct window_node *src_node, struct window *dst_window, CGPoint point)
{
    CGRect  f    = dst_window->frame;
    CGPoint wp   = { point.x - f.origin.x, point.y - f.origin.y };
    CGRect  c    = {{ 0.25f * f.size.width, 0.25f * f.size.height }, { 0.50f * f.size.width, 0.50f * f.size.height }};
    CGPoint t[3] = {{ 0.0f, 0.0f}, { 0.5f * f.size.width, 0.5f * f.size.height }, { f.size.width, 0.0f }};
    CGPoint r[3] = {{ f.size.width, 0.0f }, { 0.5f * f.size.width, 0.5f * f.size.height }, { f.size.width, f.size.height }};
    CGPoint b[3] = {{ f.size.width, f.size.height }, { 0.5f * f.size.width, 0.5f * f.size.height }, { 0.0f, f.size.height }};
    CGPoint l[3] = {{ 0.0f, f.size.height }, { 0.5f * f.size.width, 0.5f * f.size.height }, { 0.0f, 0.0f }};

    if ((CGRectContainsPoint(c, wp)) && (!src_node || src_node->window_count == 1)) {
        return ms->drop_action == MOUSE_MODE_STACK ? MOUSE_DROP_ACTION_STACK : MOUSE_DROP_ACTION_SWAP;
    } else if (triangle_contains_point(t, wp)) {
        return MOUSE_DROP_ACTION_WARP_TOP;
    } else if (triangle_contains_point(r, wp)) {
        return MOUSE_DROP_ACTION_WARP_RIGHT;
    } else if (triangle_contains_point(b, wp)) {
        return MOUSE_DROP_ACTION_WARP_BOTTOM;
    } else if (triangle_contains_point(l, wp)) {
        return MOUSE_DROP_ACTION_WARP_LEFT;
    }

    return MOUSE_DROP_ACTION_NONE;
}

void mouse_drop_action_stack(struct window_manager *wm, struct view *src_view, struct window *src_window, struct view *dst_view, struct window *dst_window)
{
    space_manager_untile_window(src_view, src_window);
    window_manager_remove_managed_window(wm, src_window->id);

    struct window_node *dst_node = view_find_window_node(dst_view, dst_window->id);
    if (dst_node->window_count+1 < NODE_MAX_WINDOW_COUNT) {
        view_stack_window_node(dst_node, src_window);
        window_manager_add_managed_window(wm, src_window, dst_view);
        window_manager_adjust_layer(src_window, LAYER_BELOW);
        scripting_addition_order_window(src_window->id, 1, dst_node->window_order[1]);

        if (dst_node->zoom) {
            window_manager_animate_window((struct window_capture) { src_window, dst_node->zoom->area.x, dst_node->zoom->area.y, dst_node->zoom->area.w, dst_node->zoom->area.h });
        } else {
            window_manager_animate_window((struct window_capture) { src_window, dst_node->area.x, dst_node->area.y, dst_node->area.w, dst_node->area.h });
        }
    }
}

void mouse_drop_action_swap(struct window_manager *wm, struct view *src_view, struct window_node *src_node, struct window *src_window, struct view *dst_view, struct window_node *dst_node, struct window *dst_window)
{
    if (window_node_contains_window(src_node, src_view->insertion_point)) {
        src_view->insertion_point = dst_window->id;
    } else if (window_node_contains_window(dst_node, dst_view->insertion_point)) {
        dst_view->insertion_point = src_window->id;
    }

    window_node_swap_window_list(src_node, dst_node);

    if (src_view->sid != dst_view->sid) {
        for (int i = 0; i < src_node->window_count; ++i) {
            window_manager_remove_managed_window(wm, src_node->window_list[i]);
            window_manager_add_managed_window(wm, window_manager_find_window(wm, src_node->window_list[i]), src_view);
        }

        for (int i = 0; i < dst_node->window_count; ++i) {
            window_manager_remove_managed_window(wm, dst_node->window_list[i]);
            window_manager_add_managed_window(wm, window_manager_find_window(wm, dst_node->window_list[i]), dst_view);
        }
    }

    struct window_capture *window_list = NULL;
    window_node_capture_windows(src_node, &window_list);
    window_node_capture_windows(dst_node, &window_list);
    window_manager_animate_window_list(window_list, ts_buf_len(window_list));
}

void mouse_drop_action_warp(struct window_manager *wm, struct view *src_view, struct window_node *src_node, struct window *src_window, struct view *dst_view, struct window_node *dst_node, struct window *dst_window, enum window_node_split split, enum window_node_child child)
{
    if ((src_node->parent && dst_node->parent) &&
        (src_node->parent == dst_node->parent) &&
        (src_node->window_count == 1)) {
        if (dst_node->parent->split == split) {
            mouse_drop_action_swap(wm, src_view, src_node, src_window, dst_view, dst_node, dst_window);
            return;
        } else {
            dst_node->parent->split = split;
            dst_node->parent->child = child;
        }
    } else {
        dst_node->split = split;
        dst_node->child = child;
    }

    struct window_node *src_node_rm = view_remove_window_node(src_view, src_window);
    window_manager_remove_managed_window(wm, src_window->id);
    window_manager_purify_window(wm, src_window);

    struct window_node *src_node_add = view_add_window_node_with_insertion_point(dst_view, src_window, dst_window->id);
    window_manager_add_managed_window(wm, src_window, dst_view);

    struct window_capture *window_list = NULL;

    if (src_node_rm) {
        window_node_capture_windows(src_node_rm, &window_list);
    }

    if (src_node_rm != src_node_add && src_node_rm != src_node_add->parent) {
        window_node_capture_windows(src_node_add, &window_list);
    }

    window_manager_animate_window_list(window_list, ts_buf_len(window_list));
}

void mouse_drop_no_target(struct space_manager *sm, struct window_manager *wm, struct view *src_view, struct view *dst_view, struct window *window, struct window_node *node)
{
    if (src_view->sid == dst_view->sid) {
        window_node_flush(node);
    } else {
        space_manager_untile_window(src_view, window);
        window_manager_remove_managed_window(wm, window->id);
        window_manager_purify_window(wm, window);

        struct view *view = space_manager_tile_window_on_space(sm, window, dst_view->sid);
        window_manager_add_managed_window(wm, window, view);
    }
}

void mouse_drop_try_adjust_bsp_grid(struct window_manager *wm, struct view *view, struct window *window, struct mouse_window_info *info)
{
    bool success = true;

    if (view->layout != VIEW_BSP) {
        success = false;
        goto end;
    }

    if (info->changed_position) {
        uint8_t direction = 0;
        if (info->changed_x) direction |= HANDLE_LEFT;
        if (info->changed_y) direction |= HANDLE_TOP;
        if (window_manager_resize_window_relative(wm, window, direction, info->dx, info->dy, true) == WINDOW_OP_ERROR_INVALID_DST_NODE) {
            success = false;
        }
    }

    if (info->changed_size) {
        uint8_t direction = 0;
        if (info->changed_w && !info->changed_x) direction |= HANDLE_RIGHT;
        if (info->changed_h && !info->changed_y) direction |= HANDLE_BOTTOM;
        if (window_manager_resize_window_relative(wm, window, direction, info->dw, info->dh, true) == WINDOW_OP_ERROR_INVALID_DST_NODE) {
            success = false;
        }
    }

end:
    if (!success) {
        struct window_node *node = view_find_window_node(view, window->id);
        if (node) window_node_flush(node);
    }
}

void mouse_show_drop_feedback(struct mouse_state *ms, struct window_node *a_node, struct window *window, struct window_node *b_node, CGPoint point)
{
    if (ms->feedback_node && ms->feedback_node != b_node) {
        ms->feedback_node->insert_dir = 0;
        insert_feedback_destroy(ms->feedback_node);
    }

    int insert_dir = 0;
    enum mouse_drop_action drop_action = mouse_determine_drop_action(ms, a_node, window, point);
    switch (drop_action) {
    case MOUSE_DROP_ACTION_STACK: {
        insert_dir = STACK;
    } break;
    case MOUSE_DROP_ACTION_SWAP: {
        insert_dir = STACK;
    } break;
    case MOUSE_DROP_ACTION_WARP_TOP: {
        insert_dir = DIR_NORTH;
    } break;
    case MOUSE_DROP_ACTION_WARP_RIGHT: {
        insert_dir = DIR_EAST;
    } break;
    case MOUSE_DROP_ACTION_WARP_BOTTOM: {
        insert_dir = DIR_SOUTH;
    } break;
    case MOUSE_DROP_ACTION_WARP_LEFT: {
        insert_dir = DIR_WEST;
    } break;
    case MOUSE_DROP_ACTION_NONE: {
        /* silence compiler warning.. */
    } break;
    }

    if (b_node->insert_dir != insert_dir) {
        b_node->insert_dir = insert_dir;
        insert_feedback_show(b_node);
        ms->feedback_node = b_node;
    }
}

void mouse_clear_drop_feedback(struct mouse_state *ms)
{
    if (ms->feedback_node) {
        ms->feedback_node->insert_dir = 0;
        insert_feedback_destroy(ms->feedback_node);
        ms->feedback_node = NULL;
    }
}

//
// NOTE(eriskii): Live layout while dragging.
//
// Upstream only re-tiles once the mouse is released. Here, a tiled window that is
// resized (by its own edges, or modifier + resize) drives its fence ratios on every
// drag event, so the neighbouring windows follow along in real-time. A tiled window
// that is moved is picked up: it is detached from the tree immediately so the rest
// of the layout fills the gap, and is inserted wherever it is dropped.
//

static bool mouse_live_resize_set_ratio(struct view *view, struct window_node *fence, float edge)
{
    int gap = view_check_flag(view, VIEW_ENABLE_GAP) ? view->window_gap : 0;

    float origin = fence->split == SPLIT_Y ? fence->area.x       : fence->area.y;
    float size   = fence->split == SPLIT_Y ? fence->area.w - gap : fence->area.h - gap;
    if (size <= 0.0f) return false;

    float ratio = clampf_range((edge - origin) / size, 0.1f, 0.9f);
    if (ratio == fence->ratio) return false;

    fence->ratio = ratio;
    return true;
}

static void mouse_live_resize_flush(struct window_node *x_fence, struct window_node *y_fence, uint32_t exclude_wid)
{
    if (x_fence && y_fence) {
        if (window_node_is_ancestor(x_fence, y_fence)) y_fence = NULL;
        else if (window_node_is_ancestor(y_fence, x_fence)) x_fence = NULL;
    }

    if (x_fence) window_node_flush_immediate(x_fence, exclude_wid);
    if (y_fence) window_node_flush_immediate(y_fence, exclude_wid);
}

void mouse_drag_decide(struct window_manager *wm, struct mouse_state *ms, CGPoint point)
{
    struct view *view = window_manager_find_managed_window(wm, ms->window);
    if (!view || view->layout != VIEW_BSP) {
        ms->drag_decided = true;
        return;
    }

    if (ms->current_action == MOUSE_MODE_RESIZE) {
        window_manager_cancel_step_animation(wm, ms->window->id);
        ms->live_resize  = true;
        ms->drag_decided = true;
        return;
    }

    //
    // NOTE(eriskii): Don't pick a window up on a few pixels of jitter; the rest of the layout
    // would close over its slot and a careless release could drop it somewhere else.
    //

    float dx = point.x - ms->down_location.x;
    float dy = point.y - ms->down_location.y;
    bool far_enough = (dx * dx + dy * dy) >= (8.0f * 8.0f);

    bool pick_up = ms->current_action == MOUSE_MODE_MOVE;
    if (pick_up && !far_enough) return;

    if (ms->current_action == MOUSE_MODE_NONE) {

        //
        // NOTE(eriskii): A plain drag could be a titlebar move, an edge resize, or just a drag
        // inside the window's content. Wait until the window server reports which one it is.
        //

        CGRect frame;
        SLSGetWindowBounds(g_connection, ms->window->id, &frame);

        if (AX_DIFF(frame.size.width, ms->down_frame.size.width) || AX_DIFF(frame.size.height, ms->down_frame.size.height)) {
            window_manager_cancel_step_animation(wm, ms->window->id);
            ms->live_resize  = true;
            ms->drag_decided = true;
            return;
        }

        if (!AX_DIFF(frame.origin.x, ms->down_frame.origin.x) && !AX_DIFF(frame.origin.y, ms->down_frame.origin.y)) return;
        if (!far_enough) return;
        pick_up = true;
    }

    ms->drag_decided = true;
    if (!pick_up) return;

    window_manager_cancel_step_animation(wm, ms->window->id);
    mouse_clear_drop_feedback(ms);

    struct window_node *flush_node = view_detach_window_node(view, ms->window, &ms->pickup);
    if (!flush_node) return;

    window_manager_remove_managed_window(wm, ms->window->id);
    window_manager_adjust_layer(ms->window, LAYER_NORMAL);
    ms->picked_up = true;

    if (space_is_visible(view->sid)) {
        window_node_flush(flush_node);
    } else {
        view_set_flag(view, VIEW_IS_DIRTY);
    }
}

void mouse_live_resize_update(struct window_manager *wm, struct mouse_state *ms, CGPoint point, bool final)
{
    if (!final) {
        float dt = ((float) read_os_timer() - ms->last_moved_time) * (1000.0f / (float)read_os_freq());
        if (dt < 16.0f) return;
    }

    struct view *view = window_manager_find_managed_window(wm, ms->window);
    if (!view || view->layout != VIEW_BSP) return;

    struct window_node *node = view_find_window_node(view, ms->window->id);
    if (!node) return;

    CGRect frame = ms->down_frame;
    uint32_t exclude_wid = 0;

    if (ms->current_action == MOUSE_MODE_RESIZE) {
        float dx = point.x - ms->down_location.x;
        float dy = point.y - ms->down_location.y;

        if (ms->direction & HANDLE_LEFT)   { frame.origin.x += dx; frame.size.width  -= dx; }
        if (ms->direction & HANDLE_RIGHT)  {                       frame.size.width  += dx; }
        if (ms->direction & HANDLE_TOP)    { frame.origin.y += dy; frame.size.height -= dy; }
        if (ms->direction & HANDLE_BOTTOM) {                       frame.size.height += dy; }
    } else {
        SLSGetWindowBounds(g_connection, ms->window->id, &frame);

        if (AX_DIFF(frame.origin.x, ms->down_frame.origin.x)) {
            ms->direction |= HANDLE_LEFT;
        } else if (AX_DIFF(CGRectGetMaxX(frame), CGRectGetMaxX(ms->down_frame))) {
            ms->direction |= HANDLE_RIGHT;
        }

        if (AX_DIFF(frame.origin.y, ms->down_frame.origin.y)) {
            ms->direction |= HANDLE_TOP;
        } else if (AX_DIFF(CGRectGetMaxY(frame), CGRectGetMaxY(ms->down_frame))) {
            ms->direction |= HANDLE_BOTTOM;
        }

        //
        // NOTE(eriskii): The application is resizing this window itself while the drag is in
        // progress; only reposition its neighbours, and snap it into its tile on release.
        //

        if (!final) exclude_wid = ms->window->id;
    }

    int gap = view_check_flag(view, VIEW_ENABLE_GAP) ? view->window_gap : 0;
    struct window_node *x_fence = NULL;
    struct window_node *y_fence = NULL;

    if (ms->direction & HANDLE_LEFT) {
        struct window_node *fence = window_node_fence(node, DIR_WEST);
        if (fence && mouse_live_resize_set_ratio(view, fence, frame.origin.x - gap)) y_fence = fence;
    } else if (ms->direction & HANDLE_RIGHT) {
        struct window_node *fence = window_node_fence(node, DIR_EAST);
        if (fence && mouse_live_resize_set_ratio(view, fence, CGRectGetMaxX(frame))) y_fence = fence;
    }

    if (ms->direction & HANDLE_TOP) {
        struct window_node *fence = window_node_fence(node, DIR_NORTH);
        if (fence && mouse_live_resize_set_ratio(view, fence, frame.origin.y - gap)) x_fence = fence;
    } else if (ms->direction & HANDLE_BOTTOM) {
        struct window_node *fence = window_node_fence(node, DIR_SOUTH);
        if (fence && mouse_live_resize_set_ratio(view, fence, CGRectGetMaxY(frame))) x_fence = fence;
    }

    if (x_fence || y_fence || final) {
        view_update(view);

        if (!space_is_visible(view->sid)) {
            view_set_flag(view, VIEW_IS_DIRTY);
        } else if (final) {
            window_node_flush_immediate(view->root, 0);
            view_clear_flag(view, VIEW_IS_DIRTY);
        } else {
            mouse_live_resize_flush(x_fence, y_fence, exclude_wid);
        }
    }

    ms->last_moved_time = read_os_timer();
}

static void mouse_find_drop_target(struct space_manager *sm, struct window_manager *wm, struct mouse_state *ms, CGPoint point, struct view **dst_view, struct window **dst_window, struct window_node **dst_node)
{
    uint64_t cursor_sid = display_space_id(display_manager_point_display_id(point));
    *dst_view = space_manager_find_view(sm, cursor_sid);

    struct window *window = window_manager_find_window_at_point_filtering_window(wm, point, ms->window->id);
    if (!window) window = window_manager_find_window_at_point(wm, point);
    if (window == ms->window) window = NULL;

    *dst_window = window;
    *dst_node = window && *dst_view ? view_find_window_node(*dst_view, window->id) : NULL;
}

void mouse_picked_up_update_feedback(struct space_manager *sm, struct window_manager *wm, struct mouse_state *ms, CGPoint point)
{
    struct view *dst_view;
    struct window *dst_window;
    struct window_node *dst_node;
    mouse_find_drop_target(sm, wm, ms, point, &dst_view, &dst_window, &dst_node);

    if (dst_node) {
        mouse_show_drop_feedback(ms, NULL, dst_window, dst_node, point);
    } else {
        mouse_clear_drop_feedback(ms);
    }
}

void mouse_drop_picked_up_window(struct space_manager *sm, struct window_manager *wm, struct mouse_state *ms, CGPoint point, bool restore)
{
    struct window *src_window = ms->window;
    struct view *src_view = space_manager_find_view(sm, ms->pickup.sid);

    struct view *dst_view = NULL;
    struct window *dst_window = NULL;
    struct window_node *dst_node = NULL;
    if (!restore) mouse_find_drop_target(sm, wm, ms, point, &dst_view, &dst_window, &dst_node);
    mouse_clear_drop_feedback(ms);

    enum mouse_drop_action drop_action = dst_node ? mouse_determine_drop_action(ms, NULL, dst_window, point) : MOUSE_DROP_ACTION_NONE;
    enum window_node_split split = SPLIT_NONE;
    enum window_node_child child = CHILD_NONE;

    switch (drop_action) {
    case MOUSE_DROP_ACTION_WARP_TOP:    { split = SPLIT_X; child = CHILD_FIRST;  } break;
    case MOUSE_DROP_ACTION_WARP_RIGHT:  { split = SPLIT_Y; child = CHILD_SECOND; } break;
    case MOUSE_DROP_ACTION_WARP_BOTTOM: { split = SPLIT_X; child = CHILD_SECOND; } break;
    case MOUSE_DROP_ACTION_WARP_LEFT:   { split = SPLIT_Y; child = CHILD_FIRST;  } break;
    default: break;
    }

    wm->suppress_animations = true;

    if (split != SPLIT_NONE) {

        //
        // NOTE(eriskii): Dropped on the edge of a window: tile it next to that window. The slot it
        // was picked up from is already gone, which is exactly what the user saw while dragging.
        //

        dst_node->split = split;
        dst_node->child = child;

        window_manager_adjust_layer(src_window, LAYER_BELOW);
        view_add_window_node_with_insertion_point(dst_view, src_window, dst_window->id);
        window_manager_add_managed_window(wm, src_window, dst_view);
    } else {

        //
        // NOTE(eriskii): Swap, stack, or no target: put the window back into the slot it came from
        // first, so the regular drop actions see the same tree they would have without a pickup.
        //

        struct window_node *src_node = view_reattach_window_node(src_view, src_window, &ms->pickup);
        if (!src_node && src_view->layout != VIEW_FLOAT) src_node = view_add_window_node(src_view, src_window);

        if (src_node) {
            window_manager_adjust_layer(src_window, LAYER_BELOW);
            window_manager_add_managed_window(wm, src_window, src_view);

            if (dst_window && dst_view) dst_node = view_find_window_node(dst_view, dst_window->id);

            if (dst_node && dst_node != src_node && drop_action == MOUSE_DROP_ACTION_SWAP) {
                mouse_drop_action_swap(wm, src_view, src_node, src_window, dst_view, dst_node, dst_window);
            } else if (dst_node && dst_node != src_node && drop_action == MOUSE_DROP_ACTION_STACK) {
                mouse_drop_action_stack(wm, src_view, src_window, dst_view, dst_window);
            } else if (dst_view && dst_view != src_view) {
                mouse_drop_no_target(sm, wm, src_view, dst_view, src_window, src_node);
            }
        }
    }

    wm->suppress_animations = false;

    view_update(src_view);
    view_flush(src_view);

    if (dst_view && dst_view != src_view) {
        view_update(dst_view);
        view_flush(dst_view);
    }
}

void mouse_state_init(struct mouse_state *state)
{
    state->modifier    = MOUSE_MOD_FN;
    state->action1     = MOUSE_MODE_MOVE;
    state->action2     = MOUSE_MODE_RESIZE;
    state->drop_action = MOUSE_MODE_SWAP;
}

bool mouse_handler_begin(struct mouse_state *mouse_state, uint32_t mask)
{
    if (mouse_state->handle) return true;

    mouse_state->handle = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, mouse_handler, mouse_state);
    if (!mouse_state->handle) return false;

    if (!CGEventTapIsEnabled(mouse_state->handle)) {
        CFMachPortInvalidate(mouse_state->handle);
        CFRelease(mouse_state->handle);
        return false;
    }

    mouse_state->runloop_source = CFMachPortCreateRunLoopSource(NULL, mouse_state->handle, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), mouse_state->runloop_source, kCFRunLoopCommonModes);

    return true;
}

void mouse_handler_end(struct mouse_state *mouse_state)
{
    if (!mouse_state->handle) return;

    CGEventTapEnable(mouse_state->handle, false);
    CFMachPortInvalidate(mouse_state->handle);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), mouse_state->runloop_source, kCFRunLoopCommonModes);
    CFRelease(mouse_state->runloop_source);
    CFRelease(mouse_state->handle);
    __atomic_store_n(&mouse_state->handle, NULL, __ATOMIC_RELEASE);
}
