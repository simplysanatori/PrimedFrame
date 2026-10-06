package org.metroidprime.port;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.CornerPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.SystemClock;
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
 * One layout, the GameCube pad's. By default there is no C-stick: a drag on the
 * free screen area aims like a mouse. The "Classic GameCube layout" setting
 * brings back the C-stick and the D-pad. The game assigns the actions.
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
    private static final float MAP_BUTTON_RADIUS_DP = 26f;
    private static final long MAP_BUTTON_POLL_MS = 200;
    // Hold-and-slide wheels that replace the D-pad: id 0 = Visor, 1 = Beam.
    private static final int WHEEL = 9;
    private static final String[] WHEEL_BUTTON_LABELS = {"Visor", "Beam"};
    // Sectors run up, right, down, left, as the stock D-pad (visors) and C-stick
    // (beams) directions do. Items are numbered as the native side does: visors
    // Combat/X-Ray/Scan/Thermal, beams Power/Ice/Wave/Plasma.
    private static final String[][] WHEEL_LABELS = {
        {"Combat", "X-Ray", "Thermal", "Scan"},
        {"Power", "Wave", "Ice", "Plasma"},
    };
    private static final int[][] WHEEL_ITEMS = {{0, 1, 3, 2}, {0, 2, 1, 3}};
    private static final int WHEEL_VALID_BIT = 1 << 12;
    private static final float WHEEL_ICON_DP = 44f;
    private static final long WHEEL_ICON_RETRY_MS = 1000;
    private static final float WHEEL_BUTTON_GAP_DP = 8f; // wheel to edge / other button
    private static final float WHEEL_BUTTON_RADIUS = 0.072f;
    private static final float WHEEL_RADIUS_DP = 112f;
    private static final float WHEEL_DEAD_DP = 30f;
    private static final long WHEEL_TAP_MS = 250;

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
    // The layout's fractions of the view height stop growing at this many dp
    // (a 500 dp phone is unchanged), so a tablet gets phone-sized controls
    // that keep their distance to the nearest edge.
    private static final float MAX_LAYOUT_DP = 520f;
    private static final float STICK_LEFT_X = 0.18f;
    private static final float STICK_Y = 0.73f;
    private static final float STICK_RADIUS = 0.16f;
    private static final float LEFT_STICK_REACH = 1.5f; // grab area, in stick radii
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

    // The GameCube pad's face, so the overlay
    // matches the pad the game was authored for: a big green A, a small red B
    // at its lower left, and X and Y as kidneys curving round A's right and top.
    private static final ControlButton[] GAMECUBE_FACE = {
        ControlButton.round("A", BTN_SOUTH, 0f, 0f, 0.085f, GC_GREEN),
        ControlButton.round("B", BTN_EAST, -0.123f, 0.103f, 0.050f, GC_RED),
        ControlButton.kidney("X", BTN_WEST, 0.158f, 0.040f, -37f, 55f, GC_GREY),
        ControlButton.kidney("Y", BTN_NORTH, 0.158f, 0.040f, -150f, 55f, GC_GREY),
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
    // goes under R here. L and R
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

    private ControlButton[] face = GAMECUBE_FACE;
    private PillButton[] pills = GAMECUBE_PILLS;
    // The GameCube pad's colours, off by default (plain translucent buttons);
    // an F1 setting, so it's re-read every draw.
    private boolean colored;
    // Classic GameCube layout (F1 setting, re-read every draw): the C-stick is
    // drawn and grabs presses in its zone.
    private boolean cStick;
    // A drag on the free area aims (mouse-style, or the classic turn and look
    // up/down). Always on unless the classic layout turns it off.
    private boolean aim;
    // Beam and visor wheels replace the D-pad. F1 settings, re-read every draw.
    private boolean wheels;
    private boolean visorTapScan;
    private int lastWheelMask;
    // The open wheel (the WHEEL target's pointer), else -1.
    private int wheelPointer = -1;
    private float wheelCx;
    private float wheelCy;
    // Tap the minimap to open the map; replaces the GameCube layout's Z pill. An
    // F1 setting, re-read every draw.
    private boolean mapTap;
    // x0, y0, x1, y1 (fractions of the view), then 1 when the minimap is drawn there.
    private final float[] minimapRect = new float[5];
    // Where the map opens but the minimap isn't drawn (the visors other than
    // Combat), a map button stands in its place. The HUD changes without a
    // touch, so a poll redraws when the button comes or goes. The same poll
    // shows and hides the wheel buttons (only drawn while the wheels work).
    private final RectF mapButtonRect = new RectF();
    private boolean mapButtonShown;
    private boolean wheelButtonsShown;
    private final Runnable mapButtonPoll = new Runnable() {
        @Override
        public void run() {
            if ((mapTap && mapButtonState(getWidth(), getHeight()) != mapButtonShown) ||
                (wheels && ((nativeWheelOwned() & WHEEL_VALID_BIT) != 0) != wheelButtonsShown)) {
                invalidate();
            } else if (mapTap || wheels) {
                postDelayed(this, MAP_BUTTON_POLL_MS);
            }
        }
    };

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
    private static native boolean nativeTouchClassic();
    private static native boolean nativeTouchColors();
    private static native boolean nativeTouchAimEnabled();
    private static native void nativeTouchAim(float dxDp, float dyDp);
    private static native void nativeTouchAimDown(boolean down);
    private static native boolean nativeTouchWheelsEnabled();
    private static native boolean nativeTouchVisorTapScan();
    // Bits 0-3 visors owned, 4-7 beams owned, 8-9 current visor, 10-11 current
    // beam, 12 valid (0 = no player: wheels disabled).
    private static native int nativeWheelOwned();
    // {width, height, ARGB pixels...} of the game's beam/visor icon, or null until the HUD has it.
    private static native int[] nativeWheelIcon(int wheel, int item);
    private static native void nativeRequestVisor(int visor);
    private static native void nativeRequestBeam(int beam);
    private static native boolean nativeTouchMapTapEnabled();
    // The minimap's screen rect as fractions of the view; false when not shown.
    private static native boolean nativeMinimapRect(float[] out5);
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
            if (wheels) {
                final int mask = nativeWheelOwned();
                if (mask != lastWheelMask) {
                    lastWheelMask = mask;
                    invalidate();
                }
            }
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
        // The classic setting can change mid-session, so re-check it every draw.
        // Off, aim and the wheels are always on.
        final boolean classic = nativeTouchClassic();
        colored = nativeTouchColors();
        cStick = classic;
        aim = !classic || nativeTouchAimEnabled();
        wheels = !classic || nativeTouchWheelsEnabled();
        visorTapScan = nativeTouchVisorTapScan();
        mapTap = nativeTouchMapTapEnabled();
        hideBounds.set(width - dp(EYE_MARGIN_DP + EYE_WIDTH_DP),
                       height - dp(EYE_MARGIN_DP + EYE_HEIGHT_DP),
                       width - dp(EYE_MARGIN_DP), height - dp(EYE_MARGIN_DP));
        drawStick(canvas, leftStickX(width, height), leftStickY(height),
                  layoutU(height) * STICK_RADIUS, leftPointer, 0);
        // The right stick is the C-stick, yellow on the GameCube pad. Only the
        // classic layout has one; otherwise a drag anywhere free aims.
        if (cStick) {
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
        if (wheels) {
            drawWheelButtons(canvas, width, height);
        } else {
            drawDpad(canvas, width, height);
        }
        drawEye(canvas, hideBounds);
        mapButtonShown = mapButtonState(width, height);
        if (mapButtonShown) {
            drawMapButton(canvas);
        }
        removeCallbacks(mapButtonPoll);
        if (mapTap || wheels) {
            postDelayed(mapButtonPoll, MAP_BUTTON_POLL_MS);
        }
        if (wheels && wheelPointer != -1) {
            drawWheel(canvas);
        }
    }

    // True, with mapButtonRect set, when the map button should show.
    private boolean mapButtonState(float width, float height) {
        if (!mapTap || width <= 0f || height <= 0f || !nativeMinimapRect(minimapRect) ||
            minimapRect[4] != 0f) {
            return false;
        }
        mapButtonRect.set(minimapRect[0] * width, minimapRect[1] * height,
                          minimapRect[2] * width, minimapRect[3] * height);
        return true;
    }

    // A round button with a folded map: three panels, the middle one raised.
    private void drawMapButton(Canvas canvas) {
        final float cx = mapButtonRect.centerX();
        final float cy = mapButtonRect.centerY();
        final float radius = Math.min(dp(MAP_BUTTON_RADIUS_DP),
                                      Math.min(mapButtonRect.width(), mapButtonRect.height()) * 0.5f);
        fillPaint.setColor(0x99081218);
        strokePaint.setColor(0xBBFFFFFF);
        canvas.drawCircle(cx, cy, radius, fillPaint);
        canvas.drawCircle(cx, cy, radius, strokePaint);
        final float w = radius * 1.1f;
        final float h = radius * 0.8f;
        final float l = cx - w / 2f;
        final float t = cy - h / 2f;
        final float tilt = h * 0.12f;
        shapePath.reset();
        shapePath.moveTo(l, t + tilt);
        shapePath.lineTo(l + w / 3f, t);
        shapePath.lineTo(l + 2f * w / 3f, t + tilt);
        shapePath.lineTo(l + w, t);
        shapePath.lineTo(l + w, t + h - tilt);
        shapePath.lineTo(l + 2f * w / 3f, t + h);
        shapePath.lineTo(l + w / 3f, t + h - tilt);
        shapePath.lineTo(l, t + h);
        shapePath.close();
        shapePath.moveTo(l + w / 3f, t);
        shapePath.lineTo(l + w / 3f, t + h - tilt);
        shapePath.moveTo(l + 2f * w / 3f, t + tilt);
        shapePath.lineTo(l + 2f * w / 3f, t + h);
        canvas.drawPath(shapePath, strokePaint);
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
                nativeSetTouchDevice(false);
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
            nativeSetTouchDevice(false);
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
                } else if (target.type == WHEEL) {
                    target.x = event.getX(i);
                    target.y = event.getY(i);
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
        removeCallbacks(mapButtonPoll);
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
            pillRect(pill, width, height, pillHit);
            if (pillHit.contains(x, y)) {
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
        // Hidden wheel buttons (wheels unusable) let the touch through.
        if (wheels && (nativeWheelOwned() & WHEEL_VALID_BIT) != 0) {
            final int wheel = wheelButtonAt(x, y, width, height);
            if (wheel != -1) {
                // Not while the map is open (the overlay is handled before this).
                if (wheelPointer == -1 && !nativeMapScreenOpen()) {
                    TouchTarget target = new TouchTarget(WHEEL, wheel);
                    target.x = x;
                    target.y = y;
                    target.startX = x;
                    target.startY = y;
                    target.startMs = SystemClock.uptimeMillis();
                    wheelCx = wheelButtonX(wheel, width, height);
                    wheelCy = wheelButtonY(height);
                    wheelPointer = pointerId;
                    targets.put(pointerId, target);
                }
                return;
            }
        }
        int dpadButton = wheels ? -1 : dpadButtonAt(x, y, width, height);
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
        if (onLeftStick(x, y, width, height) && leftPointer == -1) {
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
        } else if (cStick && x >= rightStickGrabLeft(width, height) &&
                   x < rightStickRight(width, height) &&
                   y > layoutYFromBottom(0.43f, height)) {
            if (rightPointer == -1) {
                TouchTarget target = new TouchTarget(RIGHT_STICK, 0);
                rightPointer = pointerId;
                targets.put(pointerId, target);
                updateStick(target, x, y);
            }
        } else if (aim && aimPointer == -1) {
            // Everything else that is free: one finger at a time aims.
            TouchTarget target = new TouchTarget(AIM, 0);
            target.x = x;
            target.y = y;
            aimPointer = pointerId;
            targets.put(pointerId, target);
            nativeTouchAimDown(true);
        }
    }

    // A press within half a stick radius outside the left stick's ring grabs it.
    private boolean onLeftStick(float x, float y, float width, float height) {
        final float dx = x - leftStickX(width, height);
        final float dy = y - leftStickY(height);
        final float reach = layoutU(height) * STICK_RADIUS * LEFT_STICK_REACH;
        return dx * dx + dy * dy <= reach * reach;
    }

    // The layout height: the view's, capped at a phone's. Fractions of the
    // height are sizes and offsets from it; positions go through the layout
    // helpers below, which keep a fraction's distance to the nearest edge,
    // scaled by u / height.
    private float layoutU(float height) {
        return Math.min(height, dp(MAX_LAYOUT_DP));
    }

    private float layoutScale(float height) {
        return height > 0f ? layoutU(height) / height : 1f;
    }

    // A horizontal fraction of the width, from the nearest side edge.
    private float layoutX(float fraction, float width, float height) {
        final float s = layoutScale(height);
        return fraction < 0.5f ? fraction * width * s
                               : fraction * width + (1f - fraction) * width * (1f - s);
    }

    // A horizontal fraction of the width round the screen's centre line.
    private float layoutXCentred(float fraction, float width, float height) {
        return fraction * width + (0.5f - fraction) * width * (1f - layoutScale(height));
    }

    // A vertical fraction of the height, from the nearest top or bottom edge.
    private float layoutY(float fraction, float height) {
        return fraction < 0.5f ? fraction * layoutU(height)
                               : layoutYFromBottom(fraction, height);
    }

    private float layoutYFromBottom(float fraction, float height) {
        return fraction * height + (1f - fraction) * (height - layoutU(height));
    }

    private float leftStickX(float width, float height) {
        return layoutX(STICK_LEFT_X, width, height);
    }

    private float leftStickY(float height) {
        return layoutY(STICK_Y, height);
    }

    private float rightStickX(float width, float height) {
        return width - layoutU(height) * GC_CSTICK_FROM_RIGHT;
    }

    private float rightStickY(float height) {
        return layoutY(GC_CSTICK_Y, height);
    }

    private float rightStickRadius(float height) {
        return layoutU(height) * GC_CSTICK_RADIUS;
    }

    // Right edge of the area that grabs the right stick. Face buttons are
    // hit-tested first, so it can reach past the C-stick.
    private float rightStickRight(float width, float height) {
        return rightStickX(width, height) + rightStickRadius(height) * 1.6f;
    }

    // Its left edge: a phone's 0.38 of the width, with the area's reach to the
    // stick scaled down on a big screen.
    private float rightStickGrabLeft(float width, float height) {
        final float right = rightStickRight(width, height);
        return right - (right - width * 0.38f) * layoutScale(height);
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
        float centreX = left ? leftStickX(width, height) : rightStickX(width, height);
        float centreY = left ? leftStickY(height) : rightStickY(height);
        float radius = left ? layoutU(height) * STICK_RADIUS : rightStickRadius(height);
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
        if (target.type == WHEEL) {
            wheelPointer = -1;
            finishWheel(target);
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
        if (target.type == WHEEL) {
            wheelPointer = -1;
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

    private float centreX(ControlButton button, float width, float height) {
        return button.anchored ? width - (GC_A_FROM_RIGHT - button.x) * layoutU(height)
                               : layoutX(button.x, width, height);
    }

    private float centreY(ControlButton button, float height) {
        return layoutY(button.anchored ? GC_A_Y + button.y : button.y, height);
    }

    private boolean hitButton(ControlButton button, float x, float y, float width, float height) {
        float dx = x - centreX(button, width, height);
        float dy = y - centreY(button, height);
        if (!button.isKidney()) {
            float radius = button.radius * layoutU(height);
            return dx * dx + dy * dy <= radius * radius;
        }
        // Distance to the nearest point of the kidney's centre arc.
        double angle = Math.toDegrees(Math.atan2(dy, dx));
        double along = ((angle - button.arcStart) % 360 + 360) % 360;
        if (along > button.arcSweep) {
            along = along - button.arcSweep < 360 - along ? button.arcSweep : 0;
        }
        double nearest = Math.toRadians(button.arcStart + along);
        float ring = button.radius * layoutU(height);
        float px = dx - ring * (float) Math.cos(nearest);
        float py = dy - ring * (float) Math.sin(nearest);
        float half = button.halfWidth * layoutU(height);
        return px * px + py * py <= half * half;
    }

    // The D-pad direction under (x, y), or -1. The whole square round the cross
    // counts, by the dominant axis, so a thumb that slips off an arm's side or
    // into a corner still presses something; only a small centre is dead.
    private int dpadButtonAt(float x, float y, float width, float height) {
        float dx = x - layoutX(DPAD_X, width, height);
        float dy = y - layoutY(DPAD_Y, height);
        float arm = DPAD_ARM * layoutU(height);
        float dead = DPAD_HALF * layoutU(height) * 0.4f;
        if (Math.abs(dx) > arm || Math.abs(dy) > arm || dx * dx + dy * dy < dead * dead) {
            return -1;
        }
        if (Math.abs(dx) > Math.abs(dy)) {
            return dx < 0 ? BTN_DPAD_LEFT : BTN_DPAD_RIGHT;
        }
        return dy < 0 ? BTN_DPAD_UP : BTN_DPAD_DOWN;
    }

    private void drawDpad(Canvas canvas, float width, float height) {
        float cx = layoutX(DPAD_X, width, height);
        float cy = layoutY(DPAD_Y, height);
        float arm = DPAD_ARM * layoutU(height);
        float half = DPAD_HALF * layoutU(height);
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
        return mapTap && pill.axis < 0 && pill.button == BTN_RIGHT_SHOULDER;
    }

    // A pill's rect: the middle ones round the centre line, the others from
    // their side edge, all from the top.
    private void pillRect(PillButton pill, float width, float height, RectF out) {
        final boolean middle = Math.abs((pill.left + pill.right) * 0.5f - 0.5f) < 0.15f;
        final float left = middle ? layoutXCentred(pill.left, width, height)
                                  : layoutX(pill.left, width, height);
        final float right = middle ? layoutXCentred(pill.right, width, height)
                                   : layoutX(pill.right, width, height);
        final float u = layoutU(height);
        out.set(left, pill.top * u, right, pill.bottom * u);
    }

    private void drawPillButton(Canvas canvas, PillButton pill, float width, float height) {
        pillRect(pill, width, height, pillHit);
        float left = pillHit.left;
        float top = pillHit.top;
        float right = pillHit.right;
        float bottom = pillHit.bottom;
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
        float radius = button.radius * layoutU(height);
        boolean active = held.containsKey(button.button);
        if (colored && button.color != 0) {
            fillPaint.setColor(padFill(button.color, active));
        } else {
            fillPaint.setColor(active ? 0xCC48C8E8 : 0x77081218);
        }
        strokePaint.setColor(active ? 0xFFE1F8FF : 0xBBFFFFFF);
        if (button.isKidney()) {
            kidneyPath(shapePath, x, y, radius, button.halfWidth * layoutU(height), button.arcStart,
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

    // The Visor (left) and Beam (right) buttons sit side by side near the bottom.
    // Each wheel opens centred on its button, so the buttons sit a wheel radius
    // up from the edge and far enough apart that a wheel never covers the other.
    // They are centred on screen, or in the gap between the sticks when the
    // C-stick is shown (classic layout).
    private float wheelButtonX(int wheel, float width, float height) {
        float centre = width * 0.5f;
        if (cStick) {
            final float left = leftStickX(width, height) + STICK_RADIUS * layoutU(height);
            final float right = rightStickX(width, height) - rightStickRadius(height);
            centre = (left + right) * 0.5f;
        }
        final float offset =
            (dp(WHEEL_RADIUS_DP) + WHEEL_BUTTON_RADIUS * layoutU(height) + dp(WHEEL_BUTTON_GAP_DP)) * 0.5f;
        return centre + (wheel == 0 ? -offset : offset);
    }

    private float wheelButtonY(float height) {
        return height - dp(WHEEL_RADIUS_DP) - dp(WHEEL_BUTTON_GAP_DP);
    }

    private int wheelButtonAt(float x, float y, float width, float height) {
        final float radius = WHEEL_BUTTON_RADIUS * layoutU(height) * 1.1f;
        for (int wheel = 0; wheel < 2; ++wheel) {
            final double dx = x - wheelButtonX(wheel, width, height);
            final double dy = y - wheelButtonY(height);
            if (dx * dx + dy * dy <= radius * radius) {
                return wheel;
            }
        }
        return -1;
    }

    // The game's own icons, decoded by the native side once the HUD has loaded them. Until one
    // arrives (or when it never does) the text label is drawn.
    private final Bitmap[][] wheelIcons = new Bitmap[2][4];
    private final Paint iconPaint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect iconSrc = new Rect();
    private final RectF iconDst = new RectF();
    private long wheelIconTryMs = -WHEEL_ICON_RETRY_MS;

    private void refreshWheelIcons() {
        final long now = SystemClock.uptimeMillis();
        if (now - wheelIconTryMs < WHEEL_ICON_RETRY_MS) {
            return;
        }
        wheelIconTryMs = now;
        for (int wheel = 0; wheel < 2; ++wheel) {
            for (int item = 0; item < 4; ++item) {
                if (wheelIcons[wheel][item] != null) {
                    continue;
                }
                final int[] raw = nativeWheelIcon(wheel, item);
                if (raw == null || raw.length < 3) {
                    continue;
                }
                final int w = raw[0];
                final int h = raw[1];
                if (w <= 0 || h <= 0 || raw.length != 2 + w * h) {
                    continue;
                }
                wheelIcons[wheel][item] = Bitmap.createBitmap(raw, 2, w, w, h, Bitmap.Config.ARGB_8888);
            }
        }
    }

    // Draws the icon fitted into a size x size box at (cx, cy); false when it has none yet.
    private boolean drawWheelIcon(Canvas canvas, int wheel, int item, float cx, float cy, float size,
                                  int alpha) {
        final Bitmap icon = wheelIcons[wheel][item];
        if (icon == null) {
            return false;
        }
        final float scale = Math.min(size / icon.getWidth(), size / icon.getHeight());
        final float w = icon.getWidth() * scale;
        final float h = icon.getHeight() * scale;
        iconSrc.set(0, 0, icon.getWidth(), icon.getHeight());
        iconDst.set(cx - w / 2f, cy - h / 2f, cx + w / 2f, cy + h / 2f);
        iconPaint.setAlpha(alpha);
        canvas.drawBitmap(icon, iconSrc, iconDst, iconPaint);
        return true;
    }

    private void drawWheelButtons(Canvas canvas, float width, float height) {
        final int mask = nativeWheelOwned();
        wheelButtonsShown = (mask & WHEEL_VALID_BIT) != 0;
        if (!wheelButtonsShown) {
            return;
        }
        refreshWheelIcons();
        for (int wheel = 0; wheel < 2; ++wheel) {
            final float cx = wheelButtonX(wheel, width, height);
            final float cy = wheelButtonY(height);
            final float radius = WHEEL_BUTTON_RADIUS * layoutU(height);
            final boolean active = wheelPointer != -1 && wheelTarget() != null &&
                                   wheelTarget().id == wheel;
            fillPaint.setColor(active ? 0xCC48C8E8 : 0x77081218);
            strokePaint.setColor(active ? 0xFFE1F8FF : 0xBBFFFFFF);
            canvas.drawCircle(cx, cy, radius, fillPaint);
            canvas.drawCircle(cx, cy, radius, strokePaint);
            final int current = wheel == 0 ? (mask >> 8) & 3 : (mask >> 10) & 3;
            if (!drawWheelIcon(canvas, wheel, current, cx, cy, radius * 1.4f, 255)) {
                drawCenteredLabel(canvas, WHEEL_BUTTON_LABELS[wheel], cx, cy, dp(13));
            }
        }
    }

    private TouchTarget wheelTarget() {
        return wheelPointer == -1 ? null : targets.get(wheelPointer);
    }

    // The sector under (x, y) of the open wheel, or -1 inside the centre.
    private int wheelSector(float x, float y) {
        final float dx = x - wheelCx;
        final float dy = y - wheelCy;
        final float dead = dp(WHEEL_DEAD_DP);
        if (dx * dx + dy * dy < dead * dead) {
            return -1;
        }
        if (Math.abs(dx) > Math.abs(dy)) {
            return dx > 0 ? 1 : 3;
        }
        return dy < 0 ? 0 : 2;
    }

    private static boolean wheelOwned(int mask, int wheel, int item) {
        return (mask & (1 << (wheel * 4 + item))) != 0;
    }

    // The wheel opens over everything: four sectors, the one under the finger lit.
    private void drawWheel(Canvas canvas) {
        final TouchTarget target = wheelTarget();
        if (target == null) {
            return;
        }
        final int mask = nativeWheelOwned();
        final int sector = wheelSector(target.x, target.y);
        final float outer = dp(WHEEL_RADIUS_DP);
        final float inner = dp(WHEEL_DEAD_DP);
        final RectF oval = new RectF(wheelCx - outer, wheelCy - outer, wheelCx + outer,
                                     wheelCy + outer);
        final RectF hole = new RectF(wheelCx - inner, wheelCy - inner, wheelCx + inner,
                                     wheelCy + inner);
        final int currentIndex = target.id == 0 ? (mask >> 8) & 3 : (mask >> 10) & 3;
        for (int i = 0; i < 4; ++i) {
            final int item = WHEEL_ITEMS[target.id][i];
            final boolean owned = wheelOwned(mask, target.id, item);
            // Sector i is centred on up (-90), right (0), down (90), left (180).
            final float start = -135f + 90f * i;
            shapePath.reset();
            shapePath.arcTo(oval, start, 90f, true);
            shapePath.arcTo(hole, start + 90f, -90f, false);
            shapePath.close();
            final boolean lit = i == sector && owned;
            fillPaint.setColor(lit ? 0xDD48C8E8 : owned ? 0xAA081218 : 0x66081218);
            canvas.drawPath(shapePath, fillPaint);
            strokePaint.setColor(item == currentIndex ? 0xFFE0C020 : 0xBBFFFFFF);
            canvas.drawPath(shapePath, strokePaint);
            // An item not found yet leaves its sector empty.
            if (!owned) {
                continue;
            }
            final double mid = Math.toRadians(start + 45f);
            final float labelR = (outer + inner) / 2f;
            final float lx = wheelCx + labelR * (float) Math.cos(mid);
            final float ly = wheelCy + labelR * (float) Math.sin(mid);
            if (!drawWheelIcon(canvas, target.id, item, lx, ly, dp(WHEEL_ICON_DP), 255)) {
                drawCenteredLabel(canvas, WHEEL_LABELS[target.id][i], lx, ly, dp(12));
            }
        }
    }

    // A finger lifted off a wheel: a slide to an owned sector picks it; a quick
    // tap on Visor picks Scan when the setting is on; the centre cancels.
    private void finishWheel(TouchTarget target) {
        final int mask = nativeWheelOwned();
        if ((mask & WHEEL_VALID_BIT) == 0) {
            return;
        }
        final int sector = wheelSector(target.x, target.y);
        if (sector >= 0) {
            final int item = WHEEL_ITEMS[target.id][sector];
            if (wheelOwned(mask, target.id, item)) {
                if (target.id == 0) {
                    nativeRequestVisor(item);
                } else {
                    nativeRequestBeam(item);
                }
            }
            return;
        }
        final boolean quick = SystemClock.uptimeMillis() - target.startMs < WHEEL_TAP_MS;
        final boolean still = Math.hypot(target.x - target.startX, target.y - target.startY) <
                              dp(MAP_TAP_SLOP_DP);
        if (target.id == 0 && visorTapScan && quick && still && wheelOwned(mask, 0, 2)) {
            nativeRequestVisor(2);
        }
    }

    private void drawCenteredLabel(Canvas canvas, String label, float x, float y, float size) {
        textPaint.setTextSize(size);
        canvas.drawText(label, x, y - (textPaint.ascent() + textPaint.descent()) / 2, textPaint);
    }

    private final RectF pillHit = new RectF();

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
        long startMs;

        TouchTarget(int type, int id) {
            this.type = type;
            this.id = id;
        }
    }
}
