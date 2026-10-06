package org.metroidprime.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.CornerPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * On-screen controller drawn over the game surface.
 *
 * It feeds an SDL virtual gamepad rather than synthesising keyboard keys, so the
 * game's own controller mapping, prompts and rebinding treat it as a real pad,
 * and its sticks are sticks rather than four keys pretending to be one.
 *
 * Two layouts: the GameCube pad's face layout while twin-stick is off, and the
 * Xbox arrangement while it is on, because that mode plays like a modern
 * shooter. Only arrangement and labels change; the game assigns the actions.
 */
final class TouchControlsView extends View {
    private static final int LEFT_STICK = 1;
    private static final int RIGHT_STICK = 2;
    private static final int BUTTON = 3;
    private static final int HIDE = 4;
    // A drag on the free screen area that turns the view by its travel.
    private static final int AIM = 5;
    // A tap on the HUD minimap, which opens the map.
    private static final int MAP_TAP = 6;
    // Travel, in dp, past which a minimap touch is a drag and not a tap.
    // A one-finger drag on the open map screen pans it.
    private static final int MAP_PAN = 7;
    // A second finger on the map: with the MAP_PAN one it pinches to zoom.
    private static final int MAP_PAN2 = 8;
    // Finger spread, in dp, under which a pinch is ignored (the ratio blows up).
    private static final float MAP_PINCH_MIN_DP = 20f;
    // Screen y points down, so atan2 grows clockwise; this maps it to the map's yaw.
    private static final float MAP_TWIST_SIGN = 1f;
    // Tells the game a finger is still down on the map, so it doesn't drift back.
    private static final long MAP_PAN_KEEPALIVE_MS = 100;
    private static final float MAP_TAP_SLOP_DP = 12f;

    // Port-only actions, not game inputs.
    private static final int TOGGLE_DEBUG_OVERLAY = -1;
    // The hide eye in the bottom-right corner, in dp.
    private static final int EYE_WIDTH_DP = 52;
    private static final int EYE_HEIGHT_DP = 36;
    private static final int EYE_MARGIN_DP = 8;
    // Axis-held controls are tracked with ids above this, to share one press map.
    private static final int AXIS_ID_BASE = 100;

    // SDL gamepad ids. The port maps these onto the GameCube pad itself: SOUTH is
    // A, EAST is B, WEST is X, NORTH is Y, Start is Start, the right shoulder is
    // Z, the D-pad is the D-pad, the left stick is the main stick, the right
    // stick is the C-stick and the triggers are L and R.
    private static final int BTN_SOUTH = 0;
    private static final int BTN_EAST = 1;
    private static final int BTN_WEST = 2;
    private static final int BTN_NORTH = 3;
    private static final int BTN_START = 6;
    private static final int BTN_LEFT_SHOULDER = 9;
    private static final int BTN_RIGHT_SHOULDER = 10;
    private static final int BTN_DPAD_UP = 11;
    private static final int BTN_DPAD_DOWN = 12;
    private static final int BTN_DPAD_LEFT = 13;
    private static final int BTN_DPAD_RIGHT = 14;

    private static final int AXIS_LEFTX = 0;
    private static final int AXIS_LEFTY = 1;
    private static final int AXIS_RIGHTX = 2;
    private static final int AXIS_RIGHTY = 3;
    private static final int AXIS_TRIGGER_L = 4;
    private static final int AXIS_TRIGGER_R = 5;

    // Sticks are drawn and read through these, so the two cannot disagree.
    private static final float STICK_LEFT_X = 0.18f;
    private static final float STICK_RIGHT_X = 0.58f;
    private static final float STICK_Y = 0.73f;
    private static final float STICK_RADIUS = 0.16f;
    private static final float STICK_DEAD_ZONE = 0.12f;
    // The GameCube C-stick is smaller than the main stick and sits just below
    // and left of the face buttons: its centre is this far from the right edge,
    // in view heights, like the face buttons' anchor.
    private static final float GC_CSTICK_FROM_RIGHT = 0.60f;
    private static final float GC_CSTICK_Y = 0.76f;
    private static final float GC_CSTICK_RADIUS = 0.115f;

    // The GameCube pad's colours, as RGB; the fill alpha follows the press state.
    private static final int GC_GREEN = 0x2FA864;
    private static final int GC_RED = 0xC8343A;
    private static final int GC_GREY = 0x8A8A94;
    private static final int GC_YELLOW = 0xE0C020;
    private static final int GC_PURPLE = 0x6A4FB0;

    // A's centre: from the right edge and from the top, in view heights. The
    // GameCube cluster is laid out in heights around it, so it keeps the pad's
    // shape whatever the screen's aspect.
    private static final float GC_A_FROM_RIGHT = 0.235f;
    private static final float GC_A_Y = 0.700f;

    // The GameCube pad's face, kept while twin-stick is off so the overlay
    // matches the pad the game was authored for: a big green A, a small red B
    // at its lower left, and X and Y as kidneys curving round A's right and top.
    private static final ControlButton[] GAMECUBE_FACE = {
        ControlButton.round("A", BTN_SOUTH, 0f, 0f, 0.085f, GC_GREEN),
        ControlButton.round("B", BTN_EAST, -0.123f, 0.103f, 0.050f, GC_RED),
        ControlButton.kidney("X", BTN_WEST, 0.158f, 0.040f, -37f, 55f, GC_GREY),
        ControlButton.kidney("Y", BTN_NORTH, 0.158f, 0.040f, -150f, 55f, GC_GREY),
    };

    // Twin-stick reads like a modern shooter pad: the Xbox diamond, all four the
    // same size, because a bigger A is a GameCube trait rather than an Xbox one.
    private static final ControlButton[] XBOX_FACE = {
        new ControlButton("A", BTN_SOUTH, 0.875f, 0.795f, 0.055f),
        new ControlButton("B", BTN_EAST, 0.950f, 0.720f, 0.055f),
        new ControlButton("X", BTN_WEST, 0.800f, 0.720f, 0.055f),
        new ControlButton("Y", BTN_NORTH, 0.875f, 0.645f, 0.055f),
    };

    // The D-pad is one cross, as on the GameCube pad: its centre as fractions of
    // the view, its arms in view heights so it stays square.
    private static final float DPAD_X = 0.115f;
    private static final float DPAD_Y = 0.335f;
    private static final float DPAD_ARM = 0.130f;  // centre to an arm's end
    private static final float DPAD_HALF = 0.045f; // an arm's half width
    // Up, down, left, right: ids, labels, and each arm's direction.
    private static final int[] DPAD_BUTTONS = {
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    };
    private static final String[] DPAD_LABELS = {"\u25B2", "\u25BC", "\u25C0", "\u25B6"};
    private static final int[] DPAD_DX = {0, 0, -1, 1};
    private static final int[] DPAD_DY = {-1, 1, 0, 0};

    // L and R are the pad's analog triggers; Z is its digital shoulder. On the
    // GameCube pad L and R are curved triggers and Z sits in front of R, so Z
    // goes under R here. In Xbox mode the shoulders stack on both sides, the
    // trigger above the bumper: LT/RT and LB/RB, where RB carries Z and LB the
    // twin-stick beam modifier, which is what the port reads the left shoulder
    // for. X-Box mode has no Z label because the pad has no Z button. L and R
    // are tall so a quick lock-on is hard to miss; L stops just above the D-pad.
    private static final PillButton[] GAMECUBE_PILLS = {
        new PillButton("L", AXIS_TRIGGER_L, -1, 0.020f, 0.030f, 0.150f, 0.190f, GC_GREY,
                       PillButton.TRIGGER_LEFT),
        new PillButton("R", AXIS_TRIGGER_R, -1, 0.850f, 0.030f, 0.980f, 0.190f, GC_GREY,
                       PillButton.TRIGGER_RIGHT),
        new PillButton("Z", -1, BTN_RIGHT_SHOULDER, 0.870f, 0.205f, 0.960f, 0.265f, GC_PURPLE),
        new PillButton("START", -1, BTN_START, 0.400f, 0.030f, 0.490f, 0.100f),
        new PillButton("MENU", -1, TOGGLE_DEBUG_OVERLAY, 0.510f, 0.030f, 0.600f, 0.100f),
    };

    private static final PillButton[] XBOX_PILLS = {
        new PillButton("LT", AXIS_TRIGGER_L, -1, 0.020f, 0.030f, 0.150f, 0.100f),
        new PillButton("LB", -1, BTN_LEFT_SHOULDER, 0.020f, 0.115f, 0.150f, 0.185f),
        new PillButton("RT", AXIS_TRIGGER_R, -1, 0.850f, 0.030f, 0.980f, 0.100f),
        new PillButton("RB", -1, BTN_RIGHT_SHOULDER, 0.850f, 0.115f, 0.980f, 0.185f),
        new PillButton("START", -1, BTN_START, 0.400f, 0.030f, 0.490f, 0.100f),
        new PillButton("MENU", -1, TOGGLE_DEBUG_OVERLAY, 0.510f, 0.030f, 0.600f, 0.100f),
    };

    private ControlButton[] face = GAMECUBE_FACE;
    private PillButton[] pills = GAMECUBE_PILLS;
    private boolean twinStickMode;
    // The GameCube pad's colours, off by default (plain translucent buttons);
    // an F1 setting, so it's re-read every draw like twin-stick.
    private boolean colored;
    // Twin stick with drag-to-aim: no right stick, a drag turns the view. An F1
    // setting, re-read every draw.
    private boolean touchAim;
    // GameCube layout: a drag on free area nudges the turn and looks up/down.
    private boolean gcLook;
    // Tap the minimap to open the map; replaces the GameCube layout's Z pill. An
    // F1 setting, re-read every draw.
    private boolean mapTap;
    private final float[] minimapRect = new float[4];

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<Integer, TouchTarget> targets = new HashMap<>();
    private final Map<Integer, Integer> held = new HashMap<>();
    // Fingers passed on to SDL; see forwardToSdl.
    private final Set<Integer> forwarded = new HashSet<>();
    private final RectF hideBounds = new RectF();
    private final Path shapePath = new Path();
    private final Path crossArmPath = new Path();
    private final CornerPathEffect triggerCorners = new CornerPathEffect(dp(8));
    private static native boolean nativeDebugOverlayVisible();
    private static native boolean nativeTwinStick();
    private static native boolean nativeTouchColors();
    private static native boolean nativeTouchAimEnabled();
    private static native void nativeTouchAim(float dxDp, float dyDp);
    private static native void nativeTouchAimDown(boolean down);
    private static native boolean nativeTouchMapTapEnabled();
    // The minimap's screen rect as fractions of the view; false when not shown.
    private static native boolean nativeMinimapRect(float[] out4);
    private static native void nativeMapTap();
    // A map-screen drag in dp (zero deltas = finger still down); the view height in dp.
    private static native void nativeMapPan(float dxDp, float dyDp, float viewHeightDp);
    // True while the map screen is open and can be panned.
    private static native boolean nativeMapScreenOpen();
    // A pinch: ratio of the finger spread now to before; above 1 zooms in.
    private static native void nativeMapZoom(float ratio);
    // A twist, in radians; positive turns the map as the stick's right does.
    private static native void nativeMapRotate(float radians);
    private static native void nativeSetTouchDevice(boolean xboxLayout);
    private static native void nativeToggleDebugOverlay();
    private static native void nativeVirtualButton(int button, boolean down);
    private static native void nativeVirtualAxis(int axis, float value);
    private static native boolean nativeTakePhysicalInput();
    private int leftPointer = -1;
    private int rightPointer = -1;
    private int aimPointer = -1;
    private int panPointer = -1;
    private int pan2Pointer = -1;
    private final Runnable panKeepAlive = new Runnable() {
        @Override
        public void run() {
            if (panPointer != -1) {
                nativeMapPan(0f, 0f, panViewDp());
                postDelayed(this, MAP_PAN_KEEPALIVE_MS);
            }
        }
    };
    private boolean hidden;
    // Hidden because a real pad, keyboard or mouse was used. Unlike HIDE, which
    // leaves a SHOW button, nothing is drawn and any touch brings them back.
    private boolean autoHidden;
    private boolean lastOverlayVisible;

    private static final long PHYSICAL_INPUT_POLL_MS = 250;
    private final Runnable physicalInputPoll = new Runnable() {
        @Override
        public void run() {
            // Always drained, so input from while the controls were already
            // hidden cannot hide them again the moment they come back.
            if (nativeTakePhysicalInput() && !hidden && !autoHidden) {
                autoHidden = true;
                releaseAll();
            }
            // The overlay can open without this view hearing of it (a pad, or
            // MENU while the first frames are slow enough that a one-off
            // redraw ran before the toggle landed), so redraw on any change.
            final boolean overlayVisible = nativeDebugOverlayVisible();
            if (overlayVisible != lastOverlayVisible) {
                lastOverlayVisible = overlayVisible;
                invalidate();
            }
            postDelayed(this, PHYSICAL_INPUT_POLL_MS);
        }
    };

    TouchControlsView(Context context) {
        super(context);
        setClickable(true);
        setFocusable(false);
        fillPaint.setStyle(Paint.Style.FILL);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(dp(2));
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setFakeBoldText(true);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        // The debug overlay is drawn into the surface below; stay out of its way.
        // It is usually closed from its own Close button, which this view never
        // hears about, so keep checking; otherwise the controls stayed invisible
        // until the next touch happened to redraw them.
        if (nativeDebugOverlayVisible()) {
            postInvalidateDelayed(150);
            return;
        }
        if (autoHidden) {
            return;
        }
        if (hidden) {
            return;
        }

        float width = getWidth();
        float height = getHeight();
        // The layout follows the twin-stick setting, which the player can change
        // mid-session, so re-check it every draw.
        if (nativeTwinStick() != twinStickMode) {
            twinStickMode = !twinStickMode;
            face = twinStickMode ? XBOX_FACE : GAMECUBE_FACE;
            pills = twinStickMode ? XBOX_PILLS : GAMECUBE_PILLS;
        }
        colored = !twinStickMode && nativeTouchColors();
        final boolean aimOn = nativeTouchAimEnabled();
        touchAim = twinStickMode && aimOn;
        gcLook = !twinStickMode && aimOn;
        mapTap = nativeTouchMapTapEnabled();
        hideBounds.set(width - dp(EYE_MARGIN_DP + EYE_WIDTH_DP),
                       height - dp(EYE_MARGIN_DP + EYE_HEIGHT_DP),
                       width - dp(EYE_MARGIN_DP), height - dp(EYE_MARGIN_DP));
        drawStick(canvas, width * STICK_LEFT_X, height * STICK_Y, height * STICK_RADIUS,
                  leftPointer, 0);
        // The right stick is the C-stick, yellow on the GameCube pad. Touch aim
        // has none: a drag anywhere free turns the view.
        if (!touchAim) {
            drawStick(canvas, rightStickX(width, height), rightStickY(height),
                      rightStickRadius(height), rightPointer, colored ? GC_YELLOW : 0);
        }

        for (PillButton pill : pills) {
            if (!pillHidden(pill)) {
                drawPillButton(canvas, pill, width, height);
            }
        }
        for (ControlButton button : face) {
            drawButton(canvas, button, width, height);
        }
        drawDpad(canvas, width, height);
        drawEye(canvas, hideBounds);
    }

    // A mouse is not a finger on the overlay. Its clicks are dispatched as
    // touches, and hover goes to the topmost hoverable view (this one, being
    // clickable), so without these the SDL surface below saw neither and a
    // click landed on whatever on-screen button was under the cursor. Declining
    // passes them down to the surface. Once SDL captures the pointer for mouse
    // aim, events go to the focused surface and never reach here.
    private static boolean fromMouse(MotionEvent event) {
        return event.getToolType(event.getActionIndex()) == MotionEvent.TOOL_TYPE_MOUSE;
    }

    @Override
    public boolean onHoverEvent(MotionEvent event) {
        return !fromMouse(event) && super.onHoverEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (fromMouse(event)) {
            return false;
        }
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();

        // While the debug overlay is open the game is paused and the touches
        // are for it, so decline them and let the SDL surface below have them.
        // Anything still held has to go first: once this view stops claiming
        // touches the matching releases never arrive, which left whatever was
        // down (a trigger, say) held for the rest of the session.
        final boolean overlayVisible = nativeDebugOverlayVisible();
        if (overlayVisible && (!targets.isEmpty() || !held.isEmpty())) {
            releaseAll();
        }
        if (action == MotionEvent.ACTION_DOWN) {
            cancelForwarded(event);
            if (overlayVisible) {
                return false;
            }
        } else if (forwardToSdl(event, action, actionIndex, overlayVisible) || overlayVisible) {
            return true;
        }

        if (autoHidden) {
            // The touch that brings them back is not also a press.
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                autoHidden = false;
                nativeTakePhysicalInput();
                nativeSetTouchDevice(twinStickMode);
                invalidate();
            }
            return true;
        }

        if (hidden) {
            // Any touch brings the controls back, and is not also a press.
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                setHidden(false);
                performClick();
            }
            return true;
        }

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            // The in-game prompts follow the input the player reached for, so
            // tell the port which set this overlay is showing.
            nativeSetTouchDevice(twinStickMode);
            int pointerId = event.getPointerId(actionIndex);
            float x = event.getX(actionIndex);
            float y = event.getY(actionIndex);
            if (hideBounds.contains(x, y)) {
                targets.put(pointerId, new TouchTarget(HIDE, 0));
                return true;
            }
            assignPointer(pointerId, x, y);
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                TouchTarget target = targets.get(event.getPointerId(i));
                if (target == null) {
                    continue;
                }
                if (target.type == LEFT_STICK || target.type == RIGHT_STICK) {
                    updateStick(target, event.getX(i), event.getY(i));
                } else if (target.type == AIM) {
                    updateAim(target, event, i);
                } else if (target.type == MAP_PAN) {
                    if (pan2Pointer == -1) {
                        updateMapPan(target, event, i);
                    }
                } else if (target.type == MAP_TAP) {
                    target.x = event.getX(i);
                    target.y = event.getY(i);
                }
            }
            if (panPointer != -1 && pan2Pointer != -1) {
                updatePinch(event);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            releasePointer(event.getPointerId(actionIndex));
        } else if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
        }
        invalidate();
        return true;
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postDelayed(physicalInputPoll, PHYSICAL_INPUT_POLL_MS);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(physicalInputPoll);
        // No more touch events will arrive to release what is held.
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    void releaseAll() {
        for (TouchTarget target : targets.values()) {
            releaseTarget(target);
        }
        targets.clear();
        held.clear();
        leftPointer = -1;
        rightPointer = -1;
        aimPointer = -1;
        panPointer = -1;
        pan2Pointer = -1;
        invalidate();
    }

    // Android sends a new finger to the view that already owns the gesture,
    // without asking whether it wants it. So while any finger that went down on
    // these controls is still held (a thumb resting on the stick, or the one that
    // tapped MENU), every tap on the debug overlay lands here rather than on the
    // SDL surface, and the overlay seems frozen until that finger lifts. Fingers
    // that go down while the overlay is open are handed to SDL directly instead,
    // and keep going there until they lift, even if the overlay closes first:
    // SDL drives its touch mouse from the first finger down and ignores every
    // other finger until that one comes up.
    //
    // Returns whether the event was entirely such fingers.
    private boolean forwardToSdl(MotionEvent event, int action, int actionIndex,
                                 boolean overlayVisible) {
        final int pointerId = event.getPointerId(actionIndex);
        switch (action) {
            case MotionEvent.ACTION_POINTER_DOWN:
                if (!overlayVisible) {
                    return false;
                }
                forwarded.add(pointerId);
                sendToSdl(event, actionIndex, MotionEvent.ACTION_DOWN);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_UP:
                if (!forwarded.remove(pointerId)) {
                    return false;
                }
                sendToSdl(event, actionIndex, MotionEvent.ACTION_UP);
                return true;
            case MotionEvent.ACTION_MOVE: {
                boolean all = true;
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (forwarded.contains(event.getPointerId(i))) {
                        sendToSdl(event, i, MotionEvent.ACTION_MOVE);
                    } else {
                        all = false;
                    }
                }
                return all;
            }
            case MotionEvent.ACTION_CANCEL:
                cancelForwarded(event);
                return false;
            default:
                return false;
        }
    }

    // A gesture that ends without its releases (cancelled, or a new one begun)
    // must still lift SDL's fingers, or its touch mouse stays stuck on them.
    private void cancelForwarded(MotionEvent event) {
        if (forwarded.isEmpty()) {
            return;
        }
        for (int pointerId : forwarded) {
            SDLActivity.onNativeTouch(event.getDeviceId(), pointerId, MotionEvent.ACTION_CANCEL,
                                      0f, 0f, 0f);
        }
        forwarded.clear();
    }

    private void sendToSdl(MotionEvent event, int index, int action) {
        // SDL wants the position normalised to its surface, which fills this
        // view's parent; this view does not while it is shrunk to SHOW.
        View parent = (View) getParent();
        if (parent == null) {
            return; // a late touch during teardown
        }
        float x = (getLeft() + event.getX(index)) / Math.max(1, parent.getWidth() - 1);
        float y = (getTop() + event.getY(index)) / Math.max(1, parent.getHeight() - 1);
        float pressure = Math.min(event.getPressure(index), 1f);
        SDLActivity.onNativeTouch(event.getDeviceId(), event.getPointerId(index), action, x, y,
                                  pressure);
    }

    private void assignPointer(int pointerId, float x, float y) {
        float width = getWidth();
        float height = getHeight();
        for (PillButton pill : pills) {
            if (pillHidden(pill)) {
                continue;
            }
            if (x >= pill.left * width && x <= pill.right * width &&
                y >= pill.top * height && y <= pill.bottom * height) {
                targets.put(pointerId, new TouchTarget(BUTTON, pill.id()));
                pressControl(pill.id());
                return;
            }
        }
        for (ControlButton button : face) {
            if (hitButton(button, x, y, width, height)) {
                targets.put(pointerId, new TouchTarget(BUTTON, button.button));
                pressControl(button.button);
                return;
            }
        }
        int dpadButton = dpadButtonAt(x, y, width, height);
        if (dpadButton != -1) {
            targets.put(pointerId, new TouchTarget(BUTTON, dpadButton));
            pressControl(dpadButton);
            return;
        }

        if (mapTap && width > 0f && height > 0f && nativeMinimapRect(minimapRect) &&
            x >= minimapRect[0] * width && x <= minimapRect[2] * width &&
            y >= minimapRect[1] * height && y <= minimapRect[3] * height) {
            TouchTarget target = new TouchTarget(MAP_TAP, 0);
            target.x = x;
            target.y = y;
            target.startX = x;
            target.startY = y;
            targets.put(pointerId, target);
            return;
        }
        if (x < width * 0.38f && y > height * 0.43f && leftPointer == -1) {
            TouchTarget target = new TouchTarget(LEFT_STICK, 0);
            leftPointer = pointerId;
            targets.put(pointerId, target);
            updateStick(target, x, y);
        } else if (nativeMapScreenOpen()) {
            // Everything else that is free pans the open map, one finger at a time.
            if (panPointer == -1) {
                TouchTarget target = new TouchTarget(MAP_PAN, 0);
                target.x = x;
                target.y = y;
                panPointer = pointerId;
                targets.put(pointerId, target);
                nativeMapPan(0f, 0f, panViewDp());
                postDelayed(panKeepAlive, MAP_PAN_KEEPALIVE_MS);
            } else if (pan2Pointer == -1) {
                TouchTarget target = new TouchTarget(MAP_PAN2, 0);
                target.x = x;
                target.y = y;
                pan2Pointer = pointerId;
                targets.put(pointerId, target);
            }
        } else if (touchAim) {
            // Everything else that is free: one finger at a time aims.
            if (aimPointer == -1) {
                TouchTarget target = new TouchTarget(AIM, 0);
                target.x = x;
                target.y = y;
                aimPointer = pointerId;
                targets.put(pointerId, target);
                nativeTouchAimDown(true);
            }
        } else if (x >= width * 0.38f && x < rightStickRight(width, height) &&
                   y > height * 0.43f) {
            if (rightPointer == -1) {
                TouchTarget target = new TouchTarget(RIGHT_STICK, 0);
                rightPointer = pointerId;
                targets.put(pointerId, target);
                updateStick(target, x, y);
            }
        } else if (gcLook && aimPointer == -1) {
            // GameCube layout: a drag on the free area turns and looks up/down.
            TouchTarget target = new TouchTarget(AIM, 0);
            target.x = x;
            target.y = y;
            aimPointer = pointerId;
            targets.put(pointerId, target);
            nativeTouchAimDown(true);
        }
    }

    private float rightStickX(float width, float height) {
        return twinStickMode ? width * STICK_RIGHT_X : width - height * GC_CSTICK_FROM_RIGHT;
    }

    private float rightStickY(float height) {
        return height * (twinStickMode ? STICK_Y : GC_CSTICK_Y);
    }

    private float rightStickRadius(float height) {
        return height * (twinStickMode ? STICK_RADIUS : GC_CSTICK_RADIUS);
    }

    // Right edge of the area that grabs the right stick. Face buttons are
    // hit-tested first, so on the GameCube layout it can reach past the C-stick.
    private float rightStickRight(float width, float height) {
        return twinStickMode ? width * 0.72f
                             : rightStickX(width, height) + rightStickRadius(height) * 1.6f;
    }

    // Sends the finger's travel since the last event, in dp, through every
    // historical sample so a fast swipe stays smooth.
    private void updateAim(TouchTarget target, MotionEvent event, int index) {
        final float density = getResources().getDisplayMetrics().density;
        float lastX = target.x;
        float lastY = target.y;
        final int history = event.getHistorySize();
        for (int h = 0; h <= history; ++h) {
            final float x = h < history ? event.getHistoricalX(index, h) : event.getX(index);
            final float y = h < history ? event.getHistoricalY(index, h) : event.getY(index);
            nativeTouchAim((x - lastX) / density, (y - lastY) / density);
            lastX = x;
            lastY = y;
        }
        target.x = lastX;
        target.y = lastY;
    }

    private float panViewDp() {
        return getHeight() / getResources().getDisplayMetrics().density;
    }

    // Like updateAim, but the deltas pan the map screen.
    private void updateMapPan(TouchTarget target, MotionEvent event, int index) {
        final float density = getResources().getDisplayMetrics().density;
        final float viewDp = panViewDp();
        float lastX = target.x;
        float lastY = target.y;
        final int history = event.getHistorySize();
        for (int h = 0; h <= history; ++h) {
            final float x = h < history ? event.getHistoricalX(index, h) : event.getX(index);
            final float y = h < history ? event.getHistoricalY(index, h) : event.getY(index);
            nativeMapPan((x - lastX) / density, (y - lastY) / density, viewDp);
            lastX = x;
            lastY = y;
        }
        target.x = lastX;
        target.y = lastY;
    }

    // Two fingers on the map: the midpoint's travel pans, the change in their
    // spread zooms. Uses the latest positions only.
    private void updatePinch(MotionEvent event) {
        final int a = event.findPointerIndex(panPointer);
        final int b = event.findPointerIndex(pan2Pointer);
        final TouchTarget ta = targets.get(panPointer);
        final TouchTarget tb = targets.get(pan2Pointer);
        if (a < 0 || b < 0 || ta == null || tb == null) {
            return;
        }
        final float density = getResources().getDisplayMetrics().density;
        final float ax = event.getX(a);
        final float ay = event.getY(a);
        final float bx = event.getX(b);
        final float by = event.getY(b);
        final float midDx = ((ax + bx) - (ta.x + tb.x)) * 0.5f / density;
        final float midDy = ((ay + by) - (ta.y + tb.y)) * 0.5f / density;
        final float beforeAngle = (float) Math.atan2(tb.y - ta.y, tb.x - ta.x);
        final float before = (float) Math.hypot(ta.x - tb.x, ta.y - tb.y) / density;
        final float now = (float) Math.hypot(ax - bx, ay - by) / density;
        ta.x = ax;
        ta.y = ay;
        tb.x = bx;
        tb.y = by;
        nativeMapPan(midDx, midDy, panViewDp());
        if (before >= MAP_PINCH_MIN_DP && now >= MAP_PINCH_MIN_DP) {
            nativeMapZoom(now / before);
            float turn = (float) Math.atan2(by - ay, bx - ax) - beforeAngle;
            if (turn > Math.PI) {
                turn -= 2f * (float) Math.PI;
            } else if (turn <= -Math.PI) {
                turn += 2f * (float) Math.PI;
            }
            nativeMapRotate(MAP_TWIST_SIGN * turn);
        }
    }

    private void updateStick(TouchTarget target, float x, float y) {
        target.x = x;
        target.y = y;
        final boolean left = target.type == LEFT_STICK;
        float width = getWidth();
        float height = getHeight();
        float centreX = left ? width * STICK_LEFT_X : rightStickX(width, height);
        float centreY = left ? height * STICK_Y : rightStickY(height);
        float radius = left ? height * STICK_RADIUS : rightStickRadius(height);
        // SDL's gamepad axes are +X right and +Y *down* (Aurora inverts Y for the
        // GameCube stick, whose +Y is up), so screen coordinates apply as-is.
        float dx = (x - centreX) / radius;
        float dy = (y - centreY) / radius;
        float length = (float) Math.hypot(dx, dy);
        if (length > 1f) {
            dx /= length;
            dy /= length;
            length = 1f;
        }
        if (length < STICK_DEAD_ZONE) {
            dx = 0f;
            dy = 0f;
        }
        nativeVirtualAxis(left ? AXIS_LEFTX : AXIS_RIGHTX, dx);
        nativeVirtualAxis(left ? AXIS_LEFTY : AXIS_RIGHTY, dy);
    }

    private void releasePointer(int pointerId) {
        TouchTarget target = targets.remove(pointerId);
        if (target == null) {
            return;
        }
        if (target.type == HIDE) {
            setHidden(true);
            performClick();
            return;
        }
        if (target.type == MAP_TAP) {
            // A tap, not a drag that began on the minimap.
            final float slop = dp(MAP_TAP_SLOP_DP);
            if (Math.hypot(target.x - target.startX, target.y - target.startY) < slop) {
                nativeMapTap();
            }
            return;
        }
        releaseTarget(target);
        if (pointerId == leftPointer) {
            leftPointer = -1;
        }
        if (pointerId == rightPointer) {
            rightPointer = -1;
        }
        if (pointerId == aimPointer) {
            aimPointer = -1;
        }
        if (pointerId == pan2Pointer) {
            pan2Pointer = -1;
        } else if (pointerId == panPointer) {
            if (pan2Pointer != -1) {
                // The other finger carries on panning from where it is.
                TouchTarget second = targets.get(pan2Pointer);
                if (second != null) {
                    TouchTarget carried = new TouchTarget(MAP_PAN, 0);
                    carried.x = second.x;
                    carried.y = second.y;
                    targets.put(pan2Pointer, carried);
                }
                panPointer = pan2Pointer;
                pan2Pointer = -1;
            } else {
                panPointer = -1;
                removeCallbacks(panKeepAlive);
            }
        }
    }

    private void releaseTarget(TouchTarget target) {
        // Nothing to zero for a hide tap, an aim drag (a distance) or a map tap
        // (fired on release only; a cancelled touch is no tap).
        if (target.type == AIM) {
            nativeTouchAimDown(false);
            return;
        }
        if (target.type == HIDE || target.type == MAP_TAP ||
            target.type == MAP_PAN || target.type == MAP_PAN2) {
            return;
        }
        if (target.type == BUTTON) {
            releaseControl(target.id);
            return;
        }
        final boolean left = target.type == LEFT_STICK;
        nativeVirtualAxis(left ? AXIS_LEFTX : AXIS_RIGHTX, 0f);
        nativeVirtualAxis(left ? AXIS_LEFTY : AXIS_RIGHTY, 0f);
    }

    private void pressControl(int id) {
        if (id == TOGGLE_DEBUG_OVERLAY) {
            // Drive the overlay directly; polling a synthetic F1 key press is
            // unreliable and depends on SDL having keyboard focus.
            if (!held.containsKey(id)) {
                held.put(id, 1);
                nativeToggleDebugOverlay();
                // The toggle lands on the next game frame; redraw after it so
                // the controls get out of the overlay's way.
                postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        invalidate();
                    }
                }, 150);
            }
            return;
        }
        int count = held.containsKey(id) ? held.get(id) : 0;
        if (count == 0) {
            if (id >= AXIS_ID_BASE) {
                nativeVirtualAxis(id - AXIS_ID_BASE, 1f);
            } else {
                nativeVirtualButton(id, true);
            }
        }
        held.put(id, count + 1);
    }

    private void releaseControl(int id) {
        Integer current = held.get(id);
        if (current == null) {
            return;
        }
        if (id == TOGGLE_DEBUG_OVERLAY) {
            held.remove(id);
            return;
        }
        if (current <= 1) {
            held.remove(id);
            if (id >= AXIS_ID_BASE) {
                // Triggers are axes, and SDL's joystick axes run -32768..32767
                // with a trigger resting at the minimum: releasing with 0 left
                // the trigger half pressed, so the game stayed locked on (and
                // strafing) after the player let go.
                nativeVirtualAxis(id - AXIS_ID_BASE, -1f);
            } else {
                nativeVirtualButton(id, false);
            }
        } else {
            held.put(id, current - 1);
        }
    }

    // Hidden, the view stays full screen but draws nothing, and the next
    // touch anywhere brings the controls back (as after a physical pad).
    private void setHidden(boolean hide) {
        releaseAll();
        hidden = hide;
        invalidate();
    }

    // color is the base's RGB, or 0 for the overlay's own.
    private void drawStick(Canvas canvas, float x, float y, float radius, int pointerId,
                           int color) {
        boolean active = pointerId != -1;
        if (color != 0) {
            fillPaint.setColor((active ? 0x88000000 : 0x55000000) | color);
        } else {
            fillPaint.setColor(active ? 0x8848C8E8 : 0x66081218);
        }
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xAAFFFFFF);
        canvas.drawCircle(x, y, radius, fillPaint);
        canvas.drawCircle(x, y, radius, strokePaint);
        float knobX = x;
        float knobY = y;
        TouchTarget target = targets.get(pointerId);
        if (target != null) {
            float dx = target.x - x;
            float dy = target.y - y;
            float distance = (float) Math.hypot(dx, dy);
            float limit = radius * 0.58f;
            if (distance > limit) {
                dx *= limit / distance;
                dy *= limit / distance;
            }
            knobX += dx;
            knobY += dy;
        }
        canvas.drawCircle(knobX, knobY, radius * 0.42f, strokePaint);
    }

    private static float centreX(ControlButton button, float width, float height) {
        return button.anchored ? width - (GC_A_FROM_RIGHT - button.x) * height
                               : button.x * width;
    }

    private static float centreY(ControlButton button, float height) {
        return (button.anchored ? GC_A_Y + button.y : button.y) * height;
    }

    private boolean hitButton(ControlButton button, float x, float y, float width, float height) {
        float dx = x - centreX(button, width, height);
        float dy = y - centreY(button, height);
        if (!button.isKidney()) {
            float radius = button.radius * height;
            return dx * dx + dy * dy <= radius * radius;
        }
        // Distance to the nearest point of the kidney's centre arc.
        double angle = Math.toDegrees(Math.atan2(dy, dx));
        double along = ((angle - button.arcStart) % 360 + 360) % 360;
        if (along > button.arcSweep) {
            along = along - button.arcSweep < 360 - along ? button.arcSweep : 0;
        }
        double nearest = Math.toRadians(button.arcStart + along);
        float ring = button.radius * height;
        float px = dx - ring * (float) Math.cos(nearest);
        float py = dy - ring * (float) Math.sin(nearest);
        float half = button.halfWidth * height;
        return px * px + py * py <= half * half;
    }

    // The D-pad direction under (x, y), or -1. The whole square round the cross
    // counts, by the dominant axis, so a thumb that slips off an arm's side or
    // into a corner still presses something; only a small centre is dead.
    private static int dpadButtonAt(float x, float y, float width, float height) {
        float dx = x - DPAD_X * width;
        float dy = y - DPAD_Y * height;
        float arm = DPAD_ARM * height;
        float dead = DPAD_HALF * height * 0.4f;
        if (Math.abs(dx) > arm || Math.abs(dy) > arm || dx * dx + dy * dy < dead * dead) {
            return -1;
        }
        if (Math.abs(dx) > Math.abs(dy)) {
            return dx < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT;
        }
        return dy < 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN;
    }

    private void drawDpad(Canvas canvas, float width, float height) {
        float cx = DPAD_X * width;
        float cy = DPAD_Y * height;
        float arm = DPAD_ARM * height;
        float half = DPAD_HALF * height;
        float corner = half * 0.35f;
        shapePath.reset();
        shapePath.addRoundRect(cx - arm, cy - half, cx + arm, cy + half, corner, corner,
                               Path.Direction.CW);
        crossArmPath.reset();
        crossArmPath.addRoundRect(cx - half, cy - arm, cx + half, cy + arm, corner, corner,
                                  Path.Direction.CW);
        shapePath.op(crossArmPath, Path.Op.UNION);
        fillPaint.setColor(colored ? padFill(GC_GREY, false) : 0x77081218);
        canvas.drawPath(shapePath, fillPaint);
        // A held arm lights from the centre out.
        for (int i = 0; i < DPAD_BUTTONS.length; ++i) {
            if (!held.containsKey(DPAD_BUTTONS[i])) {
                continue;
            }
            float endX = cx + DPAD_DX[i] * arm;
            float endY = cy + DPAD_DY[i] * arm;
            RectF bounds = new RectF(
                Math.min(cx, endX) - (DPAD_DX[i] == 0 ? half : 0),
                Math.min(cy, endY) - (DPAD_DY[i] == 0 ? half : 0),
                Math.max(cx, endX) + (DPAD_DX[i] == 0 ? half : 0),
                Math.max(cy, endY) + (DPAD_DY[i] == 0 ? half : 0));
            fillPaint.setColor(colored ? padFill(GC_GREY, true) : 0xCC48C8E8);
            canvas.drawRoundRect(bounds, corner, corner, fillPaint);
        }
        strokePaint.setColor(0xBBFFFFFF);
        canvas.drawPath(shapePath, strokePaint);
        for (int i = 0; i < DPAD_BUTTONS.length; ++i) {
            drawCenteredLabel(canvas, DPAD_LABELS[i], cx + DPAD_DX[i] * arm * 0.62f,
                              cy + DPAD_DY[i] * arm * 0.62f, dp(12));
        }
    }

    // A band round (cx, cy) along the arc, with round ends: GameCube X and Y.
    private static void kidneyPath(Path path, float cx, float cy, float ring, float half,
                                   float start, float sweep) {
        float end = start + sweep;
        float endX = cx + ring * (float) Math.cos(Math.toRadians(end));
        float endY = cy + ring * (float) Math.sin(Math.toRadians(end));
        float startX = cx + ring * (float) Math.cos(Math.toRadians(start));
        float startY = cy + ring * (float) Math.sin(Math.toRadians(start));
        path.reset();
        path.arcTo(new RectF(cx - ring - half, cy - ring - half, cx + ring + half,
                             cy + ring + half), start, sweep, true);
        path.arcTo(new RectF(endX - half, endY - half, endX + half, endY + half),
                   end, 180f, false);
        path.arcTo(new RectF(cx - ring + half, cy - ring + half, cx + ring - half,
                             cy + ring - half), end, -sweep, false);
        path.arcTo(new RectF(startX - half, startY - half, startX + half, startY + half),
                   start + 180f, 180f, false);
        path.close();
    }

    // A GameCube colour as a fill: translucent at rest, lighter and more solid
    // while held.
    private static int padFill(int rgb, boolean active) {
        if (!active) {
            return 0x99000000 | rgb;
        }
        int r = (rgb >> 16) & 0xFF;
        int g = (rgb >> 8) & 0xFF;
        int b = rgb & 0xFF;
        r += (255 - r) * 2 / 5;
        g += (255 - g) * 2 / 5;
        b += (255 - b) * 2 / 5;
        return 0xDD000000 | (r << 16) | (g << 8) | b;
    }

    // The GameCube layout's Z pill only opens the map, which a minimap tap does.
    private boolean pillHidden(PillButton pill) {
        return mapTap && !twinStickMode && pill.axis < 0 && pill.button == BTN_RIGHT_SHOULDER;
    }

    private void drawPillButton(Canvas canvas, PillButton pill, float width, float height) {
        float left = pill.left * width;
        float top = pill.top * height;
        float right = pill.right * width;
        float bottom = pill.bottom * height;
        boolean active = held.containsKey(pill.id());
        if (colored && pill.color != 0) {
            fillPaint.setColor(padFill(pill.color, active));
        } else {
            fillPaint.setColor(active ? 0xCC48C8E8 : 0x99081218);
        }
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xCCFFFFFF);
        RectF bounds = new RectF(left, top, right, bottom);
        if (pill.trigger != PillButton.FLAT) {
            drawTrigger(canvas, pill, bounds);
            return;
        }
        float radius = Math.min(bounds.width(), bounds.height()) * 0.28f;
        canvas.drawRoundRect(bounds, radius, radius, fillPaint);
        canvas.drawRoundRect(bounds, radius, radius, strokePaint);
        drawCenteredLabel(canvas, pill.label, bounds.centerX(), bounds.centerY(),
                          pill.label.length() > 2 ? dp(11) : dp(13));
    }

    // A GameCube trigger: a curved band across the top of its bounds whose inner
    // end (towards the screen's middle) drops, like the pad's shoulder. The
    // paints are already set; the whole bounds stay the touch area.
    private void drawTrigger(Canvas canvas, PillButton pill, RectF bounds) {
        float thick = bounds.height() * 0.60f;
        float drop = bounds.height() - thick;
        boolean leftSide = pill.trigger == PillButton.TRIGGER_LEFT;
        float outer = leftSide ? bounds.left : bounds.right;
        float inner = leftSide ? bounds.right : bounds.left;
        float bend = outer + (inner - outer) * 0.35f;
        shapePath.reset();
        shapePath.moveTo(outer, bounds.top);
        shapePath.quadTo(bend, bounds.top, inner, bounds.top + drop);
        shapePath.lineTo(inner, bounds.bottom);
        shapePath.quadTo(bend, bounds.top + thick, outer, bounds.top + thick);
        shapePath.close();
        fillPaint.setPathEffect(triggerCorners);
        strokePaint.setPathEffect(triggerCorners);
        canvas.drawPath(shapePath, fillPaint);
        canvas.drawPath(shapePath, strokePaint);
        fillPaint.setPathEffect(null);
        strokePaint.setPathEffect(null);
        drawCenteredLabel(canvas, pill.label, bounds.centerX(),
                          bounds.top + thick / 2 + drop * 0.4f, dp(13));
    }

    private void drawButton(Canvas canvas, ControlButton button, float width, float height) {
        float x = centreX(button, width, height);
        float y = centreY(button, height);
        float radius = button.radius * height;
        boolean active = held.containsKey(button.button);
        if (colored && button.color != 0) {
            fillPaint.setColor(padFill(button.color, active));
        } else {
            fillPaint.setColor(active ? 0xCC48C8E8 : 0x77081218);
        }
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xBBFFFFFF);
        if (button.isKidney()) {
            kidneyPath(shapePath, x, y, radius, button.halfWidth * height, button.arcStart,
                       button.arcSweep);
            canvas.drawPath(shapePath, fillPaint);
            canvas.drawPath(shapePath, strokePaint);
            double middle = Math.toRadians(button.arcStart + button.arcSweep / 2);
            x += radius * (float) Math.cos(middle);
            y += radius * (float) Math.sin(middle);
        } else {
            canvas.drawCircle(x, y, radius, fillPaint);
            canvas.drawCircle(x, y, radius, strokePaint);
        }
        drawCenteredLabel(canvas, button.label, x, y,
                          button.label.length() > 2 ? dp(10) : dp(15));
    }

    // The hide button: an eye.
    private void drawEye(Canvas canvas, RectF bounds) {
        fillPaint.setColor(0x99081218);
        strokePaint.setColor(0xCCFFFFFF);
        float corner = Math.min(bounds.width(), bounds.height()) * 0.3f;
        canvas.drawRoundRect(bounds, corner, corner, fillPaint);
        float cx = bounds.centerX();
        float cy = bounds.centerY();
        float halfW = bounds.height() * 0.48f;
        float lid = bounds.height() * 0.30f;
        Path eye = shapePath;
        eye.reset();
        eye.moveTo(cx - halfW, cy);
        eye.quadTo(cx, cy - lid * 2f, cx + halfW, cy);
        eye.quadTo(cx, cy + lid * 2f, cx - halfW, cy);
        eye.close();
        canvas.drawPath(eye, strokePaint);
        fillPaint.setColor(0xCCFFFFFF);
        canvas.drawCircle(cx, cy, lid * 0.55f, fillPaint);
    }

    private void drawCenteredLabel(Canvas canvas, String label, float x, float y, float size) {
        textPaint.setTextSize(size);
        canvas.drawText(label, x, y - (textPaint.ascent() + textPaint.descent()) / 2, textPaint);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static final class ControlButton {
        final String label;
        final int button;
        // A plain button's x is a fraction of the width and its y one of the
        // height. An anchored one's are offsets from GameCube A, in heights.
        final boolean anchored;
        final float x;
        final float y;
        // A kidney's radius is its centre arc's, round its x/y.
        final float radius;
        // Kidneys only: the band's half width, and its arc in degrees,
        // clockwise from +x. A zero sweep is a round button.
        final float halfWidth;
        final float arcStart;
        final float arcSweep;
        // An RGB fill, or 0 for the overlay's own.
        final int color;

        ControlButton(String label, int button, float x, float y, float radius) {
            this(label, button, false, x, y, radius, 0f, 0f, 0f, 0);
        }

        private ControlButton(String label, int button, boolean anchored, float x, float y,
                              float radius, float halfWidth, float arcStart, float arcSweep,
                              int color) {
            this.label = label;
            this.button = button;
            this.anchored = anchored;
            this.x = x;
            this.y = y;
            this.radius = radius;
            this.halfWidth = halfWidth;
            this.arcStart = arcStart;
            this.arcSweep = arcSweep;
            this.color = color;
        }

        static ControlButton round(String label, int button, float x, float y, float radius,
                                   int color) {
            return new ControlButton(label, button, true, x, y, radius, 0f, 0f, 0f, color);
        }

        static ControlButton kidney(String label, int button, float ring, float halfWidth,
                                    float arcStart, float arcSweep, int color) {
            return new ControlButton(label, button, true, 0f, 0f, ring, halfWidth, arcStart,
                                     arcSweep, color);
        }

        boolean isKidney() {
            return arcSweep != 0f;
        }
    }

    private static final class PillButton {
        final String label;
        final int axis;
        final int button;
        final float left;
        final float top;
        final float right;
        final float bottom;
        // An RGB fill, or 0 for the overlay's own.
        final int color;
        // FLAT is a rounded pill; the others draw a GameCube trigger.
        static final int FLAT = 0;
        static final int TRIGGER_LEFT = 1;
        static final int TRIGGER_RIGHT = 2;
        final int trigger;

        PillButton(String label, int axis, int button, float left, float top, float right,
                   float bottom) {
            this(label, axis, button, left, top, right, bottom, 0, FLAT);
        }

        PillButton(String label, int axis, int button, float left, float top, float right,
                   float bottom, int color) {
            this(label, axis, button, left, top, right, bottom, color, FLAT);
        }

        PillButton(String label, int axis, int button, float left, float top, float right,
                   float bottom, int color, int trigger) {
            this.color = color;
            this.trigger = trigger;
            this.label = label;
            this.axis = axis;
            this.button = button;
            this.left = left;
            this.top = top;
            this.right = right;
            this.bottom = bottom;
        }

        // One identity for the press map and the held-highlight, whether this is
        // a button or an analog trigger sent as an axis.
        int id() {
            return axis >= 0 ? AXIS_ID_BASE + axis : button;
        }
    }

    private static final class TouchTarget {
        final int type;
        final int id;
        float x;
        float y;
        float startX;
        float startY;

        TouchTarget(int type, int id) {
            this.type = type;
            this.id = id;
        }
    }
}
