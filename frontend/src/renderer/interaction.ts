// Full interaction set (M17 "Interaction"): wheel/pinch zoom (anchored under the cursor via
// coords.ts's zoomAnchoredStartFrame — never re-derived here), drag-to-select on the waveform,
// drag-to-scrub on the ruler, middle-drag/space-drag pan, click-to-seek, double-click-select, and
// keyboard navigation. Owns no drawing — it only computes ViewActions/selection/seek targets and
// hands them to the callbacks the app loop supplies.

import { clamp, pixelToFrame } from './coords.ts';
import type { ViewAction, ViewLimits, ViewState } from './viewState.ts';
import type { HoverFeedback } from './playheadOverlay.ts';

export interface SelectionRange {
  startFrame: number;
  endFrame: number;
}

export interface MarkerLike {
  id: string;
  frame: number;
}

export interface InteractionCallbacks {
  getView(): ViewState;
  getLimits(): ViewLimits;
  getMarkers(): readonly MarkerLike[];
  dispatch(action: ViewAction): void;
  onSeek(frame: number): void;
  onScrub?(frame: number): void;
  onSelectionChange(range: SelectionRange | null): void;
  onHover(hover: HoverFeedback | null): void;
  onDragGhost(dragGhostFrame: number | null): void;
  onTogglePlay(): void;
  onReCentre(): void;
  formatHoverLabel?(frame: number): string;
}

export interface InteractionOptions {
  /** CSS-pixel height of the ruler strip at the top, where drags scrub instead of selecting. */
  rulerHeightCss: number;
  /** Wheel zoom requires this modifier when true (the doc's "ctrl/cmd modifier, configurable");
   *  when false, plain wheel zooms and shift+wheel (or a trackpad's natural horizontal delta)
   *  pans. Defaults to true, matching most DAWs/editors. */
  wheelZoomRequiresModifier?: boolean;
  wheelZoomStep?: number;
  clickDragThresholdCss?: number;
  keyboardNudgeFraction?: number; // fraction of the visible span per arrow-key press
  keyboardZoomFactor?: number;
}

const kDefaultWheelZoomStep = 1.2;
const kDefaultClickDragThresholdCss = 3;
const kDefaultKeyboardNudgeFraction = 0.01;
const kDefaultKeyboardZoomFactor = 1.3;

type PointerRole = 'pan' | 'select' | 'scrub' | 'pinch';

interface ActivePointer {
  id: number;
  role: PointerRole;
  startClientX: number;
  startClientY: number;
  lastClientX: number;
  lastClientY: number;
  startView: ViewState;
  moved: boolean;
}

/** Attaches every M17 interaction gesture to `el`. `el` must be focusable (tabindex="0" or
 *  greater) for the keyboard handlers to fire; this class does not set that attribute itself so
 *  the host page controls its own focus order. Returns a `dispose()` to remove all listeners. */
export function attachInteraction(el: HTMLElement, opts: InteractionOptions, cb: InteractionCallbacks): { dispose(): void } {
  const wheelZoomStep = opts.wheelZoomStep ?? kDefaultWheelZoomStep;
  const requiresModifier = opts.wheelZoomRequiresModifier ?? true;
  const dragThreshold = opts.clickDragThresholdCss ?? kDefaultClickDragThresholdCss;
  const nudgeFraction = opts.keyboardNudgeFraction ?? kDefaultKeyboardNudgeFraction;
  const keyboardZoomFactor = opts.keyboardZoomFactor ?? kDefaultKeyboardZoomFactor;

  const activePointers = new Map<number, ActivePointer>();
  let spaceHeld = false;
  let pinchStartDistance = 0;
  let pinchStartFpp = 0;

  const rect = (): DOMRect => el.getBoundingClientRect();
  const clientXToPixel = (clientX: number): number => clientX - rect().left;
  const clientYToPixel = (clientY: number): number => clientY - rect().top;

  function frameAtClientX(view: ViewState, clientX: number): number {
    return pixelToFrame(clientXToPixel(clientX), view);
  }

  function roleForPointerDown(event: PointerEvent): PointerRole {
    if (event.button === 1 || spaceHeld) return 'pan';
    if (clientYToPixel(event.clientY) < opts.rulerHeightCss) return 'scrub';
    return 'select';
  }

  function onWheel(event: WheelEvent): void {
    const view = cb.getView();
    const hasModifier = event.ctrlKey || event.metaKey;
    const wantsZoom = requiresModifier ? hasModifier : !event.shiftKey;
    event.preventDefault();

    if (wantsZoom) {
      const anchorPixel = clientXToPixel(event.clientX);
      const factor = event.deltaY > 0 ? wheelZoomStep : 1 / wheelZoomStep;
      cb.dispatch({ type: 'zoomAt', anchorPixel, factor });
      return;
    }
    const deltaCss = requiresModifier && event.shiftKey ? event.deltaY : event.deltaX || event.deltaY;
    cb.dispatch({ type: 'pan', deltaFrames: deltaCss * view.framesPerPixel });
  }

  function onPointerDown(event: PointerEvent): void {
    el.setPointerCapture(event.pointerId);
    const otherPointers = [...activePointers.values()];
    if (otherPointers.length === 1) {
      // Second touch landing -> switch both into a pinch gesture.
      const first = otherPointers[0]!;
      first.role = 'pinch';
      activePointers.set(event.pointerId, {
        id: event.pointerId,
        role: 'pinch',
        startClientX: event.clientX,
        startClientY: event.clientY,
        lastClientX: event.clientX,
        lastClientY: event.clientY,
        startView: cb.getView(),
        moved: false,
      });
      pinchStartDistance = Math.max(1, Math.hypot(first.lastClientX - event.clientX, first.lastClientY - event.clientY));
      pinchStartFpp = cb.getView().framesPerPixel;
      return;
    }

    const role = roleForPointerDown(event);
    activePointers.set(event.pointerId, {
      id: event.pointerId,
      role,
      startClientX: event.clientX,
      startClientY: event.clientY,
      lastClientX: event.clientX,
      lastClientY: event.clientY,
      startView: cb.getView(),
      moved: false,
    });
  }

  function onPointerMove(event: PointerEvent): void {
    const pointer = activePointers.get(event.pointerId);
    if (!pointer) {
      // Not dragging: surface hover feedback (M17 layer 6).
      const view = cb.getView();
      const frame = frameAtClientX(view, event.clientX);
      cb.onHover({
        xCss: clientXToPixel(event.clientX),
        yCss: clientYToPixel(event.clientY),
        ...(cb.formatHoverLabel ? { label: cb.formatHoverLabel(frame) } : {}),
      });
      return;
    }

    if (pointer.role === 'pinch') {
      pointer.lastClientX = event.clientX;
      pointer.lastClientY = event.clientY;
      const other = [...activePointers.values()].find((p) => p.id !== pointer.id && p.role === 'pinch');
      if (!other) return;

      const currentDistance = Math.max(1, Math.hypot(pointer.lastClientX - other.lastClientX, pointer.lastClientY - other.lastClientY));
      const framesPerPixel = pinchStartFpp * (pinchStartDistance / currentDistance);
      const anchorClientX = (pointer.lastClientX + other.lastClientX) / 2;
      cb.dispatch({ type: 'setFramesPerPixel', framesPerPixel, anchorPixel: clientXToPixel(anchorClientX) });
      return;
    }

    const dxCss = event.clientX - pointer.startClientX;
    const dyCss = event.clientY - pointer.startClientY;
    if (!pointer.moved && Math.hypot(dxCss, dyCss) > dragThreshold) pointer.moved = true;

    if (pointer.role === 'pan') {
      cb.dispatch({ type: 'pan', deltaFrames: -dxCss * pointer.startView.framesPerPixel });
      // Reset the drag origin to the current point each move so successive deltas don't compound
      // against a stale startView after the view itself has shifted.
      pointer.startClientX = event.clientX;
      pointer.startClientY = event.clientY;
      pointer.startView = cb.getView();
      return;
    }

    if (pointer.role === 'scrub') {
      const frame = frameAtClientX(cb.getView(), event.clientX);
      cb.onScrub?.(frame);
      cb.onDragGhost(frame);
      return;
    }

    if (pointer.role === 'select' && pointer.moved) {
      const view = cb.getView();
      const startFrame = frameAtClientX(view, pointer.startClientX);
      const endFrame = frameAtClientX(view, event.clientX);
      cb.onSelectionChange({ startFrame: Math.min(startFrame, endFrame), endFrame: Math.max(startFrame, endFrame) });
      cb.onDragGhost(endFrame);
    }
  }

  function onPointerUp(event: PointerEvent): void {
    const pointer = activePointers.get(event.pointerId);
    activePointers.delete(event.pointerId);
    el.releasePointerCapture(event.pointerId);
    if (!pointer) return;
    cb.onDragGhost(null);

    if (!pointer.moved && (pointer.role === 'select' || pointer.role === 'scrub')) {
      const frame = frameAtClientX(cb.getView(), event.clientX);
      cb.onSeek(frame);
      cb.onSelectionChange(null);
    }

    if (pointer.role === 'pinch') {
      const remaining = [...activePointers.values()];
      if (remaining.length === 1) {
        const survivor = remaining[0]!;
        survivor.role = 'select';
        survivor.startClientX = survivor.lastClientX;
        survivor.startClientY = survivor.lastClientY;
        survivor.startView = cb.getView();
        survivor.moved = false;
      }
    }
  }

  function onDblClick(event: MouseEvent): void {
    const view = cb.getView();
    const frame = frameAtClientX(view, event.clientX);
    const markers = [...cb.getMarkers()].sort((a, b) => a.frame - b.frame);
    const before = markers.filter((m) => m.frame <= frame).at(-1);
    const after = markers.find((m) => m.frame >= frame);
    if (before && after && before.id !== after.id) {
      cb.onSelectionChange({ startFrame: before.frame, endFrame: after.frame });
    }
    // No markers straddling the click point: nothing to select between (M18 owns markers; M17
    // just wires the gesture up so it's a no-op rather than a crash until markers exist).
  }

  function onKeyDown(event: KeyboardEvent): void {
    const view = cb.getView();
    const limits = cb.getLimits();
    const nudgeFrames = view.framesPerPixel * view.widthCss * nudgeFraction;

    switch (event.key) {
      case 'ArrowLeft':
      case 'ArrowRight': {
        event.preventDefault();
        const sign = event.key === 'ArrowLeft' ? -1 : 1;
        const amount = (event.shiftKey ? 10 : 1) * nudgeFrames;
        cb.dispatch({ type: 'pan', deltaFrames: sign * amount });
        break;
      }
      case '+':
      case '=':
        event.preventDefault();
        cb.dispatch({ type: 'zoomAt', anchorPixel: view.widthCss / 2, factor: 1 / keyboardZoomFactor });
        break;
      case '-':
      case '_':
        event.preventDefault();
        cb.dispatch({ type: 'zoomAt', anchorPixel: view.widthCss / 2, factor: keyboardZoomFactor });
        break;
      case 'Home':
        event.preventDefault();
        cb.dispatch({ type: 'panToStart', startFrame: 0 });
        cb.onSeek(0);
        break;
      case 'End':
        event.preventDefault();
        cb.dispatch({ type: 'panToStart', startFrame: limits.totalFrames - view.framesPerPixel * view.widthCss });
        cb.onSeek(limits.totalFrames);
        break;
      case ' ':
        event.preventDefault();
        cb.onTogglePlay();
        break;
      case 'Escape':
        cb.onSelectionChange(null);
        break;
    }
  }

  function onKeyUpGlobal(event: KeyboardEvent): void {
    if (event.key === ' ') spaceHeld = false;
  }
  function onKeyDownGlobal(event: KeyboardEvent): void {
    if (event.key === ' ') spaceHeld = true;
  }

  function onPointerLeave(): void {
    cb.onHover(null);
  }

  el.addEventListener('wheel', onWheel, { passive: false });
  el.addEventListener('pointerdown', onPointerDown);
  el.addEventListener('pointermove', onPointerMove);
  el.addEventListener('pointerup', onPointerUp);
  el.addEventListener('pointercancel', onPointerUp);
  el.addEventListener('pointerleave', onPointerLeave);
  el.addEventListener('dblclick', onDblClick);
  el.addEventListener('keydown', onKeyDown);
  window.addEventListener('keydown', onKeyDownGlobal);
  window.addEventListener('keyup', onKeyUpGlobal);

  return {
    dispose(): void {
      el.removeEventListener('wheel', onWheel);
      el.removeEventListener('pointerdown', onPointerDown);
      el.removeEventListener('pointermove', onPointerMove);
      el.removeEventListener('pointerup', onPointerUp);
      el.removeEventListener('pointercancel', onPointerUp);
      el.removeEventListener('pointerleave', onPointerLeave);
      el.removeEventListener('dblclick', onDblClick);
      el.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keydown', onKeyDownGlobal);
      window.removeEventListener('keyup', onKeyUpGlobal);
    },
  };
}

export function clampSelectionToTrack(selection: SelectionRange, totalFrames: number): SelectionRange {
  return {
    startFrame: clamp(selection.startFrame, 0, totalFrames),
    endFrame: clamp(selection.endFrame, 0, totalFrames),
  };
}
