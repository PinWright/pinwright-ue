# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Tracking shim for the editor callbacks a Python script registers.

`unreal.register_slate_post_tick_callback(fn)` returns a handle and the matching
`unregister_*` call is the only way to remove the callback. A `python.execute` call
runs its script in a namespace that is discarded when the call returns, so the handle
dies with the call while the callback keeps running -- through PIE teardown and through
every later request in a shared editor. The engine keeps its own handle lists
(`PySlateUtil::PythonPreTickCallbackHandles` / `PythonPostTickCallbackHandles` in
PySlate.cpp) but they are file-scope statics in a Private engine module, so no plugin
can enumerate or clear them: wrapping the registration functions is the only route.

install() replaces the three register/unregister pairs `unreal` exposes with wrappers
that keep the returned handle alive here and mirror the metadata into C++ through
`unreal.PinWrightPythonCallbackLibrary`, where `python.callbacks` reads it. The
wrappers are transparent: they return exactly what the engine returned, and a callback
that raises still raises so the engine logs it as before.

Only callbacks registered AFTER install() runs are visible; anything registered earlier
in the session (a startup script, another tool) is untracked and unclearable, which is
why `python.callbacks list` reports when tracking started.
"""

import traceback

import unreal

# current id -> {"handle", "unregister", "kind", "source", "id"}. Holding the handle here is
# the whole point: it is the object the caller lost. "id" is a ONE-ELEMENT LIST, not a
# string, because the tick wrapper reads its id through that cell -- see _readopt(), which
# rewrites it when a reloaded C++ registry re-files the callback under a new id.
_ENTRIES = {}

# name -> the engine function this module replaced on the `unreal` module.
_ORIGINALS = {}

# (registry kind, register function name, unregister function name)
_KINDS = (
    ("slate_post_tick", "register_slate_post_tick_callback", "unregister_slate_post_tick_callback"),
    ("slate_pre_tick", "register_slate_pre_tick_callback", "unregister_slate_pre_tick_callback"),
    ("python_shutdown", "register_python_shutdown_callback", "unregister_python_shutdown_callback"),
)


def _report_registered(kind, source):
    return unreal.PinWrightPythonCallbackLibrary.notify_registered(kind, source)


def _report_unregistered(callback_id):
    unreal.PinWrightPythonCallbackLibrary.notify_unregistered(callback_id)


def _report_invoked(callback_id, error_text):
    # Swallowing is deliberate here and nowhere else in this module: this runs inside a
    # tick callback, so a raise would turn the tracker into a second source of the
    # per-frame exception spam it exists to make visible.
    try:
        unreal.PinWrightPythonCallbackLibrary.notify_invoked(callback_id, error_text)
    except BaseException:
        pass


def _describe_caller():
    """`file:line in function` of the frame that called the register function."""
    try:
        for frame in reversed(traceback.extract_stack()):
            if frame.filename != __file__:
                return "%s:%d in %s" % (frame.filename, frame.lineno, frame.name)
    except BaseException:
        pass
    return "<unknown>"


def _wrap_callable(id_cell, callback):
    def _tracked(*args, **kwargs):
        try:
            result = callback(*args, **kwargs)
        except BaseException:
            _report_invoked(id_cell[0], traceback.format_exc())
            raise
        _report_invoked(id_cell[0], "")
        return result

    return _tracked


def _make_register(kind, original, unregister_name):
    def _register(callback, *args, **kwargs):
        source = _describe_caller()
        try:
            callback_id = _report_registered(kind, source)
        except BaseException:
            # The reflected library is this module's only channel into C++. If it has gone
            # (module unloaded, reflection not generated), register the callback untracked
            # rather than breaking a registration that would have worked without us.
            return original(callback, *args, **kwargs)
        id_cell = [callback_id]
        try:
            handle = original(_wrap_callable(id_cell, callback), *args, **kwargs)
        except BaseException:
            # Drop the record we just made, without letting the cleanup replace the
            # engine's own error with one of ours.
            try:
                _report_unregistered(callback_id)
            except BaseException:
                pass
            raise
        _ENTRIES[callback_id] = {
            "handle": handle,
            "unregister": unregister_name,
            "kind": kind,
            "source": source,
            "id": id_cell,
        }
        return handle

    _register._pinwright_tracked = True
    return _register


def _forget(handle):
    """Drop the entry whose handle the caller just unregistered themselves."""
    for callback_id, entry in list(_ENTRIES.items()):
        stored = entry["handle"]
        matched = stored is handle
        if not matched:
            # _DelegateHandle equality is not part of the documented Python surface, so
            # identity is tried first and a comparison that raises is not a failure.
            try:
                matched = bool(stored == handle)
            except BaseException:
                matched = False
        if matched:
            del _ENTRIES[callback_id]
            try:
                _report_unregistered(callback_id)
            except BaseException:
                pass
            return


def _make_unregister(original):
    def _unregister(handle, *args, **kwargs):
        result = original(handle, *args, **kwargs)
        _forget(handle)
        return result

    _unregister._pinwright_tracked = True
    return _unregister


def _readopt():
    """Re-file callbacks this module still holds into a C++ registry that has lost them.

    This module lives in the interpreter and survives a PinWright DLL unload/reload; the C++
    records do not. Without this pass, install() would short-circuit on the wrapper markers
    and every pre-reload callback would be running, invisible and unclearable - the exact
    state the module exists to prevent. Entries C++ still knows about are left alone, so the
    pass is a no-op on the ordinary first install and cannot duplicate a record.
    """
    for old_id in list(_ENTRIES):
        entry = _ENTRIES[old_id]
        try:
            if unreal.PinWrightPythonCallbackLibrary.is_tracked(old_id):
                continue
            new_id = _report_registered(entry["kind"], entry["source"])
        except BaseException:
            continue
        if not new_id or new_id == old_id:
            continue
        entry["id"][0] = new_id
        _ENTRIES[new_id] = entry
        del _ENTRIES[old_id]


def install():
    """Wrap the register/unregister pairs. Idempotent; returns True when tracking is live."""
    # Raise rather than install a tracker with no channel into C++: the caller reads a
    # failed install as "tracking unavailable" and reports that, where a silent success
    # would promise a listing that can never contain anything.
    if not hasattr(unreal, "PinWrightPythonCallbackLibrary"):
        raise RuntimeError(
            "unreal.PinWrightPythonCallbackLibrary is missing, so PinWright cannot record "
            "the callbacks a script registers")

    for kind, register_name, unregister_name in _KINDS:
        original_register = getattr(unreal, register_name, None)
        original_unregister = getattr(unreal, unregister_name, None)
        if original_register is None or original_unregister is None:
            continue
        if getattr(original_register, "_pinwright_tracked", False):
            continue
        _ORIGINALS[register_name] = original_register
        _ORIGINALS[unregister_name] = original_unregister
        setattr(unreal, register_name, _make_register(kind, original_register, unregister_name))
        setattr(unreal, unregister_name, _make_unregister(original_unregister))

    _readopt()
    return True


def clear(callback_ids):
    """Unregister the named callbacks. Returns the ids that were actually removed."""
    cleared = []
    for callback_id in list(callback_ids):
        entry = _ENTRIES.get(callback_id)
        if entry is None:
            continue
        handle = entry["handle"]
        original = _ORIGINALS.get(entry["unregister"])
        if original is None:
            continue
        try:
            original(handle)
        except BaseException:
            unreal.log_warning(
                "PinWright could not unregister callback %s: %s"
                % (callback_id, traceback.format_exc()))
            continue
        del _ENTRIES[callback_id]
        _report_unregistered(callback_id)
        cleared.append(callback_id)
    return cleared
