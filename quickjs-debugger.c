#include "quickjs-debugger.h"
#include <time.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct DebuggerSuspendedState {
    uint32_t variable_reference_count;
    JSValue variable_references;
    JSValue variable_pointers;
} DebuggerSuspendedState;

static char *debug_address = NULL;
static int need_load_debug_address = 1;


static char* js_get_debug_address() {
    if (!need_load_debug_address) {
        return debug_address;
    }

    return debug_address = getenv("QUICKJS_DEBUG_ADDRESS");
}

static int js_transport_read_fully(JSDebuggerInfo *info, char *buffer, size_t length) {
    int offset = 0;
    while (offset < length) {
        int received = info->transport_read(info->transport_udata, buffer + offset, length - offset);
        if (received <= 0)
            return 0;
        offset += received;
    }

    return 1;
}

static int js_transport_write_fully(JSDebuggerInfo *info, const char *buffer, size_t length) {
    int offset = 0;
    while (offset < length) {
        int sent = info->transport_write(info->transport_udata, buffer + offset, length - offset);
        if (sent <= 0)
            return 0;
        offset += sent;
    }

    return 1;
}

static int js_transport_write_message(JSDebuggerInfo *info, const char* value, size_t len) {
    int message_length = htonl(len);
    if (!js_transport_write_fully(info, (const char *)&message_length, sizeof(message_length)))
        return 0;
    return js_transport_write_fully(info, value, len);
}

static int js_transport_write_value(JSDebuggerInfo *info, JSValue value) {
    JSValue stringified = js_debugger_json_stringify(info->ctx, value);
    size_t len;
    const char* str = JS_ToCStringLen(info->ctx, &len, stringified);
    int ret = js_transport_write_message(info, str, len);
    JS_FreeCString(info->ctx, str);
    JS_FreeValue(info->ctx, stringified);
    JS_FreeValue(info->ctx, value);
    return ret;
}

static JSValue js_transport_new_envelope(JSDebuggerInfo *info, const char *type) {
    JSValue ret = JS_NewObject(info->ctx);
    JS_SetPropertyStr(info->ctx, ret, "type", JS_NewString(info->ctx, type));
    return ret;
}

static int js_transport_send_event(JSDebuggerInfo *info, JSValue event) {
    JSValue envelope = js_transport_new_envelope(info, "event");
    JS_SetPropertyStr(info->ctx, envelope, "event", event);
    return js_transport_write_value(info, envelope);
}

static int js_transport_send_response(JSDebuggerInfo *info, JSValue request, JSValue body) {
    JSContext *ctx = info->ctx;
    JSValue envelope = js_transport_new_envelope(info, "response");
    JS_SetPropertyStr(ctx, envelope, "body", body);
    JS_SetPropertyStr(ctx, envelope, "request_seq", JS_GetPropertyStr(ctx, request, "request_seq"));
    return js_transport_write_value(info, envelope);
}

static JSValue js_get_scopes(JSContext *ctx, int frame) {
    // for now this is always the same.
    // global, local, closure. may change in the future. can check if closure is empty.

    JSValue scopes = JS_NewArray(ctx);

    int scope_count = 0;

    JSValue local = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, local, "name", JS_NewString(ctx, "Local"));
    JS_SetPropertyStr(ctx, local, "reference", JS_NewInt32(ctx, (frame << 2) + 1));
    JS_SetPropertyStr(ctx, local, "expensive", JS_FALSE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, local);


    JSValue closure = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, closure, "name", JS_NewString(ctx, "Closure"));
    JS_SetPropertyStr(ctx, closure, "reference", JS_NewInt32(ctx, (frame << 2) + 2));
    JS_SetPropertyStr(ctx, closure, "expensive", JS_FALSE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, closure);

    JSValue global = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, "name", JS_NewString(ctx, "Global"));
    JS_SetPropertyStr(ctx, global, "reference", JS_NewInt32(ctx, (frame << 2) + 0));
    JS_SetPropertyStr(ctx, global, "expensive", JS_TRUE);
    JS_SetPropertyUint32(ctx, scopes, scope_count++, global);

    return scopes;
}

static JSValue js_debugger_get_variable(JSContext *ctx,
    struct DebuggerSuspendedState *state,
    JSValue var_name, JSValue var_val) {
    JSValue var = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, var, "name", var_name);
    JS_SetPropertyStr(ctx, var, "value", JS_ToString(ctx, var_val));
    // 0 means not expandible
    uint32_t reference = 0;
    if (JS_IsString(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "string"));
    else if (JS_IsInteger(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "integer"));
    else if (JS_IsNumber(var_val) || JS_IsBigFloat(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "float"));
    else if (JS_IsBool(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "boolean"));
    else if (JS_IsNull(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "null"));
    else if (JS_IsUndefined(var_val))
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "undefined"));
    else if (JS_IsObject(var_val)) {
        JS_SetPropertyStr(ctx, var, "type", JS_NewString(ctx, "object"));

        JSObject *p = JS_VALUE_GET_OBJ(var_val);
        // todo: xor the the two dwords to get a better hash?
        uint32_t pl = (uint32_t)p;
        JSValue found = JS_GetPropertyUint32(ctx, state->variable_pointers, pl);
        if (JS_IsUndefined(found)) {
            reference = state->variable_reference_count++;
            JS_SetPropertyUint32(ctx, state->variable_references, reference, JS_DupValue(ctx, var_val));
            JS_SetPropertyUint32(ctx, state->variable_pointers, pl, JS_DupValue(ctx, var_val));
        }
        else
            JS_ToUint32(ctx, &reference, found);
        JS_FreeValue(ctx, found);
    }
    JS_SetPropertyStr(ctx, var, "variablesReference", JS_NewInt32(ctx, reference));

    return var;
}

static int js_process_request(JSDebuggerInfo *info, struct DebuggerSuspendedState *state, JSValue request) {
    JSContext *ctx = info->ctx;
    JSValue command_property = JS_GetPropertyStr(ctx, request, "command");
    const char *command = JS_ToCString(ctx, command_property);
    int ret = 1;
    if (strcmp("continue", command) == 0) {
        js_transport_send_response(info, request, JS_UNDEFINED);
        ret = 0;
    }
    else if (strcmp("next", command) == 0) {
        info->stepping = 1;
        info->step_over = js_debugger_current_location(ctx);
        js_transport_send_response(info, request, JS_UNDEFINED);
        ret = 0;
    }
    else if (strcmp("stackTrace", command) == 0) {
        JSValue stack_trace = js_debugger_build_backtrace(ctx);
        js_transport_send_response(info, request, stack_trace);
    }
    else if (strcmp("scopes", command) == 0) {
        JSValue args = JS_GetPropertyStr(ctx, request, "args");
        JSValue reference_property = JS_GetPropertyStr(ctx, args, "frameId");
        JS_FreeValue(ctx, args);
        int frame;
        JS_ToInt32(ctx, &frame, reference_property);
        JS_FreeValue(ctx, reference_property);
        JSValue scopes = js_get_scopes(ctx, frame);
        js_transport_send_response(info, request, scopes);
    }
    else if (strcmp("variables", command) == 0) {
        JSValue args = JS_GetPropertyStr(ctx, request, "args");
        JSValue reference_property = JS_GetPropertyStr(ctx, args, "variablesReference");
        JS_FreeValue(ctx, args);
        uint32_t reference;
        JS_ToUint32(ctx, &reference, reference_property);
        JS_FreeValue(ctx, reference_property);

        JSValue variable = JS_GetPropertyUint32(ctx, state->variable_references, reference);

        // if the variable reference was not found,
        // then it must be a frame locals, frame closures, or the global
        if (JS_IsUndefined(variable)) {
            int frame = reference >> 2;
            int scope = reference % 4;

            assert(frame < js_debugger_stack_depth(ctx));

            if (scope == 0)
                variable = js_debugger_global_variables(ctx);
            else if (scope == 1)
                variable = js_debugger_local_variables(ctx, frame);
            else if (scope == 2)
                variable = js_debugger_closure_variables(ctx, frame);
            else
                assert(0);

            // need to dupe the variable, as it's used below as well.
            JS_SetPropertyUint32(ctx, state->variable_references, reference, JS_DupValue(ctx, variable));
        }

        JSValue properties = JS_NewArray(ctx);
        JSPropertyEnum *tab_atom;
        uint32_t tab_atom_count;
        if (!JS_GetOwnPropertyNames(ctx, &tab_atom, &tab_atom_count, variable,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {

            for(int i = 0; i < tab_atom_count; i++) {
                JSValue value = JS_GetProperty(ctx, variable, tab_atom[i].atom);
                JSValue variable_json = js_debugger_get_variable(ctx, state, JS_AtomToString(ctx, tab_atom[i].atom), value);
                JS_FreeValue(ctx, value);
                JS_SetPropertyUint32(ctx, properties, i, variable_json);
            }
        }

        JS_FreeValue(ctx, variable);

        js_transport_send_response(info, request, properties);
    }
    JS_FreeCString(ctx, command);
    JS_FreeValue(ctx, command_property);
    JS_FreeValue(ctx, request);
    return ret;
}

static void js_process_breakpoints(JSDebuggerInfo *info, JSValue message) {
    JSContext *ctx = info->ctx;

    // force all functions to reprocess their breakpoints.
    info->breakpoints_dirty_counter++;

    JSValue path_property = JS_GetPropertyStr(ctx, message, "path");
    const char *path = JS_ToCString(ctx, path_property);
    JSValue path_data = JS_GetPropertyStr(ctx, info->breakpoints, path);

    if (!JS_IsUndefined(path_data))
        JS_FreeValue(ctx, path_data);
    // use an object to store the breakpoints as a sparse array, basically.
    // this will get resolved into a pc array mirror when its detected as dirty.
    path_data = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info->breakpoints, path, path_data);
    JS_FreeCString(ctx, path);
    JS_FreeValue(ctx, path_property);

    JSValue breakpoints = JS_GetPropertyStr(ctx, message, "breakpoints");
    JS_SetPropertyStr(ctx, path_data, "breakpoints", breakpoints);
    JS_SetPropertyStr(ctx, path_data, "dirty", JS_NewInt32(ctx, info->breakpoints_dirty_counter));

    JS_FreeValue(ctx, message);
}

JSValue js_debugger_file_breakpoints(JSContext *ctx, const char* path) {
    JSDebuggerInfo *info = js_debugger_info(ctx);
    JSValue path_data = JS_GetPropertyStr(ctx, info->breakpoints, path);
    return path_data;    
}

static int js_process_debugger_messages(JSDebuggerInfo *info) {
    // continue processing messages until the continue message is received.
    JSContext *ctx = info->ctx;
    struct DebuggerSuspendedState state;
    state.variable_reference_count = js_debugger_stack_depth(ctx) << 2;
    state.variable_pointers = JS_NewObject(ctx);
    state.variable_references = JS_NewObject(ctx);
    int done_processing = 0;
    int ret = 0;

    while (!done_processing) {
        int message_length;
        if (!js_transport_read_fully(info, (char *)&message_length, sizeof(message_length)))
            goto done;

        message_length = ntohl(message_length);
        if (message_length > info->message_buffer_length) {
            if (info->message_buffer) {
                js_free(ctx, info->message_buffer);
                info->message_buffer = NULL;
            }

            // extra for null termination (debugger inspect, etc)
            info->message_buffer = js_malloc(ctx, message_length + 1);
            info->message_buffer_length = message_length;
        }

        if (!js_transport_read_fully(info, info->message_buffer, message_length))
            goto done;
        
        info->message_buffer[message_length] = '\0';

        JSValue message = JS_ParseJSON(ctx, info->message_buffer, message_length, "<debugger>");
        const char *type = JS_ToCString(ctx, JS_GetPropertyStr(ctx, message, "type"));
        if (strcmp("request", type) == 0) {
            done_processing = !js_process_request(info, &state, JS_GetPropertyStr(ctx, message, "request"));
            // done_processing = 1;
        }
        else if (strcmp("continue", type) == 0) {
            done_processing = 1;
        }
        else if (strcmp("breakpoints", type) == 0) {
            js_process_breakpoints(info, JS_GetPropertyStr(ctx, message, "breakpoints"));
        }
        JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, message);
    }
    ret = 1;

done:
    JS_FreeValue(ctx, state.variable_references);
    JS_FreeValue(ctx, state.variable_pointers);
    return ret;
}

static void js_send_stopped_event(JSDebuggerInfo *info, const char *reason) {
    JSContext *ctx = info->ctx;

    JSValue event = JS_NewObject(ctx);
    // better thread id?
    JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "StoppedEvent"));
    JS_SetPropertyStr(ctx, event, "reason", JS_NewString(ctx, reason));
    JS_SetPropertyStr(ctx, event, "thread", JS_NewInt64(ctx, (int64_t)ctx));
    js_transport_send_event(info, event);
}

// in thread check request/response of pending commands.
// todo: background thread that reads the socket.
void js_debugger_check(JSContext* ctx, JSDebuggerInfo *info) {
    if (info->is_debugging)
        return;
    info->is_debugging = 1;

    if (!info->attempted_connect) {
        info->attempted_connect = 1;
        char *address = js_get_debug_address();
        if (address != NULL && !info->transport_close)
            js_debugger_connect(ctx, address);
    }

    if (info->transport_close == NULL)
        goto done;

    int at_breakpoint = js_debugger_check_breakpoint(ctx, info->breakpoints_dirty_counter);
    if (at_breakpoint) {
        js_send_stopped_event(info, "breakpoint");
    }
    else if (info->stepping) {
        struct JSDebuggerLocation location = js_debugger_current_location(ctx);
        if (location.filename == info->step_over.filename
            && location.line == info->step_over.line
            && location.column == info->step_over.column)
            goto done;
        info->stepping = 0;
        js_send_stopped_event(info, "step");
    }
    else {
        // only peek at the stream every now and then.
        if (info->peek_ticks++ < 10000)
            goto done;

        info->peek_ticks = 0;

        int peek = info->transport_peek(info->transport_udata);
        if (peek < 0)
            goto fail;
        if (peek == 0)
            goto done;
    }

    if (js_process_debugger_messages(info))
        goto done;

    fail: 
        js_debugger_free(ctx, info);
    done:
        info->is_debugging = 0;
}

void js_debugger_free(JSContext *ctx, JSDebuggerInfo *info) {
    if (!info->transport_close)
        return;

    info->transport_close(ctx, info->transport_udata);

    info->transport_read = NULL;
    info->transport_write = NULL;
    info->transport_peek = NULL;
    info->transport_close = NULL;

    JS_FreeValue(ctx, info->breakpoints);
}

void js_debugger_attach(
    JSContext *ctx,
    size_t (*transport_read)(void *udata, char* buffer, size_t length),
    size_t (*transport_write)(void *udata, const char* buffer, size_t length),
    size_t (*transport_peek)(void *udata),
    void (*transport_close)(JSContext* ctx, void *udata),
    void *udata
) {
    JSDebuggerInfo *info = js_debugger_info(ctx);
    js_debugger_free(ctx, info);

    info->ctx = ctx;
    info->transport_read = transport_read;
    info->transport_write = transport_write;
    info->transport_peek = transport_peek;
    info->transport_close = transport_close;
    info->transport_udata = udata;

    js_send_stopped_event(info, "entry");

    info->breakpoints = JS_NewObject(ctx);

    js_process_debugger_messages(info);
}
