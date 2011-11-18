/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <config.h>

#include "function.h"
#include "arg.h"
#include "object.h"
#include "boxed.h"
#include "union.h"
#include <gjs/gjs-module.h>
#include <gjs/compat.h>

#include <util/log.h>

#include <jsapi.h>

#include <girepository.h>
#include <girffi.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* We use guint8 for arguments; functions can't
 * have more than this.
 */
#define GJS_ARG_INDEX_INVALID G_MAXUINT8

typedef enum {
    PARAM_NORMAL,
    PARAM_SKIPPED,
    PARAM_ARRAY,
    PARAM_CALLBACK
} ParamType;

typedef struct {
    GIFunctionInfo *info;

    ParamType *param_types;

    guint8 expected_js_argc;
    guint8 js_out_argc;
    GIFunctionInvoker invoker;
} Function;

static struct JSClass gjs_function_class;

/* Because we can't free the mmap'd data for a callback
 * while it's in use, this list keeps track of ones that
 * will be freed the next time we invoke a C function.
 */
static GSList *completed_trampolines = NULL;  /* GjsCallbackTrampoline */

typedef struct {
    gint ref_count;
    JSRuntime *runtime;
    GICallableInfo *info;
    jsval js_function;
    ffi_cif cif;
    ffi_closure *closure;
    GIScopeType scope;
} GjsCallbackTrampoline;

GJS_DEFINE_PRIV_FROM_JS(Function, gjs_function_class)

/*
 * Like JSResolveOp, but flags provide contextual information as follows:
 *
 *  JSRESOLVE_QUALIFIED   a qualified property id: obj.id or obj[id], not id
 *  JSRESOLVE_ASSIGNING   obj[id] is on the left-hand side of an assignment
 *  JSRESOLVE_DETECTING   'if (o.p)...' or similar detection opcode sequence
 *  JSRESOLVE_DECLARING   var, const, or function prolog declaration opcode
 *  JSRESOLVE_CLASSNAME   class name used when constructing
 *
 * The *objp out parameter, on success, should be null to indicate that id
 * was not resolved; and non-null, referring to obj or one of its prototypes,
 * if id was resolved.
 */
static JSBool
function_new_resolve(JSContext *context,
                     JSObject  *obj,
                     jsid       id,
                     uintN      flags,
                     JSObject **objp)
{
    Function *priv;
    char *name;

    *objp = NULL;

    if (!gjs_get_string_id(context, id, &name))
        return JS_TRUE; /* not resolved, but no error */

    priv = priv_from_js(context, obj);

    gjs_debug_jsprop(GJS_DEBUG_GFUNCTION, "Resolve prop '%s' hook obj %p priv %p", name, obj, priv);
    g_free(name);

    if (priv == NULL)
        return JS_TRUE; /* we are the prototype, or have the wrong class */

    return JS_TRUE;
}

static void
gjs_callback_trampoline_ref(GjsCallbackTrampoline *trampoline)
{
    trampoline->ref_count++;
}

static void
gjs_callback_trampoline_unref(GjsCallbackTrampoline *trampoline)
{
    /* Not MT-safe, like all the rest of GJS */

    trampoline->ref_count--;
    if (trampoline->ref_count == 0) {
        JSContext *context;

        context = gjs_runtime_get_current_context(trampoline->runtime);

        JS_RemoveValueRoot(context, &trampoline->js_function);
        g_callable_info_free_closure(trampoline->info, trampoline->closure);
        g_base_info_unref( (GIBaseInfo*) trampoline->info);
        g_slice_free(GjsCallbackTrampoline, trampoline);
    }
}


/* This is our main entry point for ffi_closure callbacks.
 * ffi_prep_closure is doing pure magic and replaces the original
 * function call with this one which gives us the ffi arguments,
 * a place to store the return value and our use data.
 * In other words, everything we need to call the JS function and
 * getting the return value back.
 */
static void
gjs_callback_closure(ffi_cif *cif,
                     void *result,
                     void **args,
                     void *data)
{
    JSContext *context;
    GjsCallbackTrampoline *trampoline;
    int i, n_args, n_jsargs;
    jsval *jsargs, rval;
    GITypeInfo ret_type;
    gboolean success = FALSE;

    trampoline = data;
    g_assert(trampoline);

    context = gjs_runtime_get_current_context(trampoline->runtime);
    JS_BeginRequest(context);

    n_args = g_callable_info_get_n_args(trampoline->info);

    g_assert(n_args >= 0);

    jsargs = (jsval*)g_newa(jsval, n_args);
    for (i = 0, n_jsargs = 0; i < n_args; i++) {
        GIArgInfo arg_info;
        GITypeInfo type_info;

        g_callable_info_load_arg(trampoline->info, i, &arg_info);
        g_arg_info_load_type(&arg_info, &type_info);

        /* Skip void * arguments */
        if (g_type_info_get_tag(&type_info) == GI_TYPE_TAG_VOID)
            continue;

        if (!gjs_value_from_g_argument(context,
                                       &jsargs[n_jsargs++],
                                       &type_info,
                                       args[i]))
            goto out;
    }

    if (!JS_CallFunctionValue(context,
                              NULL,
                              trampoline->js_function,
                              n_jsargs,
                              jsargs,
                              &rval)) {
        goto out;
    }

    g_callable_info_load_return_type(trampoline->info, &ret_type);

    if (!gjs_value_to_g_argument(context,
                                 rval,
                                 &ret_type,
                                 "callback",
                                 GJS_ARGUMENT_RETURN_VALUE,
                                 FALSE,
                                 TRUE,
                                 result)) {
        goto out;
    }

    success = TRUE;

out:
    if (!success) {
        gjs_log_exception (context, NULL);

        /* Fill in the result with some hopefully neutral value */
        g_callable_info_load_return_type(trampoline->info, &ret_type);
        gjs_g_argument_init_default (context, &ret_type, result);
    }

    if (trampoline->scope == GI_SCOPE_TYPE_ASYNC) {
        completed_trampolines = g_slist_prepend(completed_trampolines, trampoline);
    }

    JS_EndRequest(context);
}

/* The global entry point for any invocations of GDestroyNotify;
 * look up the callback through the user_data and then free it.
 */
static void
gjs_destroy_notify_callback(gpointer data)
{
    GjsCallbackTrampoline *trampoline = data;

    g_assert(trampoline);
    gjs_callback_trampoline_unref(trampoline);
}

static GjsCallbackTrampoline*
gjs_callback_trampoline_new(JSContext      *context,
                            jsval           function,
                            GICallableInfo *callable_info,
                            GIScopeType     scope)
{
    GjsCallbackTrampoline *trampoline;

    if (function == JSVAL_NULL) {
        return NULL;
    }

    g_assert(JS_TypeOfValue(context, function) == JSTYPE_FUNCTION);

    trampoline = g_slice_new(GjsCallbackTrampoline);
    trampoline->ref_count = 1;
    trampoline->runtime = JS_GetRuntime(context);
    trampoline->info = callable_info;
    g_base_info_ref((GIBaseInfo*)trampoline->info);
    trampoline->js_function = function;
    JS_AddValueRoot(context, &trampoline->js_function);
    trampoline->closure = g_callable_info_prepare_closure(callable_info, &trampoline->cif,
                                                          gjs_callback_closure, trampoline);

    trampoline->scope = scope;

    return trampoline;
}

/* an helper function to retrieve array lengths from a GArgument
   (letting the compiler generate good instructions in case of
   big endian machines) */
static unsigned long
get_length_from_arg (GArgument *arg, GITypeTag tag)
{
    switch (tag) {
    case GI_TYPE_TAG_INT8:
        return arg->v_int8;
    case GI_TYPE_TAG_UINT8:
        return arg->v_uint8;
    case GI_TYPE_TAG_INT16:
        return arg->v_int16;
    case GI_TYPE_TAG_UINT16:
        return arg->v_uint16;
    case GI_TYPE_TAG_INT32:
        return arg->v_int32;
    case GI_TYPE_TAG_UINT32:
        return arg->v_uint32;
    case GI_TYPE_TAG_INT64:
        return arg->v_int64;
    case GI_TYPE_TAG_UINT64:
        return arg->v_uint64;
    default:
        g_assert_not_reached ();
    }
}

static JSBool
gjs_invoke_c_function(JSContext      *context,
                      Function       *function,
                      JSObject       *obj, /* "this" object */
                      uintN           js_argc,
                      jsval          *js_argv,
                      jsval          *js_rval)
{
    /* These first four are arrays which hold argument pointers.
     * @in_arg_cvalues: C values which are passed on input (in or inout)
     * @out_arg_cvalues: C values which are returned as arguments (out or inout)
     * @inout_original_arg_cvalues: For the special case of (inout) args, we need to
     *  keep track of the original values we passed into the function, in case we
     *  need to free it.
     * @ffi_arg_pointers: For passing data to FFI, we need to create another layer
     *  of indirection; this array is a pointer to an element in in_arg_cvalues
     *  or out_arg_cvalues.
     * @return_value: The actual return value of the C function, i.e. not an (out) param
     */
    GArgument *in_arg_cvalues;
    GArgument *out_arg_cvalues;
    GArgument *inout_original_arg_cvalues;
    gpointer *ffi_arg_pointers;
    GArgument return_value;

    guint8 processed_c_args = 0;
    guint8 gi_argc, gi_arg_pos;
    guint8 c_argc, c_arg_pos;
    guint8 js_arg_pos;
    gboolean can_throw_gerror;
    gboolean did_throw_gerror = FALSE;
    GError *local_error = NULL;
    gboolean failed, postinvoke_release_failed;

    GIFunctionInfoFlags flags;
    gboolean is_method;
    GITypeInfo return_info;
    GITypeTag return_tag;
    jsval *return_values = NULL;
    guint8 next_rval = 0; /* index into return_values */
    GSList *iter;

    /* Because we can't free a closure while we're in it, we defer
     * freeing until the next time a C function is invoked.  What
     * we should really do instead is queue it for a GC thread.
     */
    if (completed_trampolines) {
        for (iter = completed_trampolines; iter; iter = iter->next) {
            GjsCallbackTrampoline *trampoline = iter->data;
            gjs_callback_trampoline_unref(trampoline);
        }
        g_slist_free(completed_trampolines);
        completed_trampolines = NULL;
    }

    flags = g_function_info_get_flags(function->info);
    is_method = (flags & GI_FUNCTION_IS_METHOD) != 0;
    can_throw_gerror = (flags & GI_FUNCTION_THROWS) != 0;
    c_argc = function->invoker.cif.nargs;
    gi_argc = g_callable_info_get_n_args( (GICallableInfo*) function->info);

    /* @c_argc is the number of arguments that the underlying C
     * function takes. @gi_argc is the number of arguments the
     * GICallableInfo describes (which does not include "this" or
     * GError**). @function->expected_js_argc is the number of
     * arguments we expect the JS function to take (which does not
     * include PARAM_SKIPPED args).
     *
     * @js_argc is the number of arguments that were actually passed;
     * we allow this to be larger than @expected_js_argc for
     * convenience, and simply ignore the extra arguments. But we
     * don't allow too few args, since that would break.
     */

    if (js_argc < function->expected_js_argc) {
        gjs_throw(context, "Too few arguments to %s %s.%s expected %d got %d",
                  is_method ? "method" : "function",
                  g_base_info_get_namespace( (GIBaseInfo*) function->info),
                  g_base_info_get_name( (GIBaseInfo*) function->info),
                  function->expected_js_argc,
                  js_argc);
        return JS_FALSE;
    }

    g_callable_info_load_return_type( (GICallableInfo*) function->info, &return_info);
    return_tag = g_type_info_get_tag(&return_info);

    in_arg_cvalues = g_newa(GArgument, c_argc);
    ffi_arg_pointers = g_newa(gpointer, c_argc);
    out_arg_cvalues = g_newa(GArgument, c_argc);
    inout_original_arg_cvalues = g_newa(GArgument, c_argc);

    failed = FALSE;
    c_arg_pos = 0; /* index into in_arg_cvalues, etc */
    gi_arg_pos = 0; /* index into function->info arguments */
    js_arg_pos = 0; /* index into argv */

    if (is_method) {
        GIBaseInfo *container = g_base_info_get_container((GIBaseInfo *) function->info);
        GIInfoType type = g_base_info_get_type(container);

        g_assert_cmpuint(0, <, c_argc);

        if (type == GI_INFO_TYPE_STRUCT || type == GI_INFO_TYPE_BOXED) {
            in_arg_cvalues[0].v_pointer = gjs_c_struct_from_boxed(context, obj);
        } else if (type == GI_INFO_TYPE_UNION) {
            in_arg_cvalues[0].v_pointer = gjs_c_union_from_union(context, obj);
        } else { /* by fallback is always object */
            GType gtype;

            in_arg_cvalues[0].v_pointer = gjs_g_object_from_object(context, obj);

            gtype = g_registered_type_info_get_g_type ((GIRegisteredTypeInfo *)container);
            if (!g_type_is_a (G_TYPE_FROM_INSTANCE (in_arg_cvalues[0].v_pointer),
                              gtype)) {
                gjs_throw(context,
                          "Expected type '%s' but got '%s'",
                          g_type_name(gtype),
                          g_type_name(G_TYPE_FROM_INSTANCE(in_arg_cvalues[0].v_pointer)));
                failed = TRUE;
                goto release;
            }
        }
        ffi_arg_pointers[0] = &in_arg_cvalues[0];
        ++c_arg_pos;
    }

    processed_c_args = c_arg_pos;
    for (gi_arg_pos = 0; gi_arg_pos < gi_argc; gi_arg_pos++, c_arg_pos++) {
        GIDirection direction;
        GIArgInfo arg_info;
        gboolean arg_removed = FALSE;

        /* gjs_debug(GJS_DEBUG_GFUNCTION, "gi_arg_pos: %d c_arg_pos: %d js_arg_pos: %d", gi_arg_pos, c_arg_pos, js_arg_pos); */

        g_callable_info_load_arg( (GICallableInfo*) function->info, gi_arg_pos, &arg_info);
        direction = g_arg_info_get_direction(&arg_info);

        g_assert_cmpuint(c_arg_pos, <, c_argc);
        ffi_arg_pointers[c_arg_pos] = &in_arg_cvalues[c_arg_pos];

        if (direction == GI_DIRECTION_OUT) {
            if (g_arg_info_is_caller_allocates(&arg_info)) {
                GITypeTag type_tag;
                GITypeInfo ainfo;

                g_arg_info_load_type(&arg_info, &ainfo);
                type_tag = g_type_info_get_tag(&ainfo);

                switch (type_tag) {
                case GI_TYPE_TAG_INTERFACE:
                    {
                        GIBaseInfo* interface_info;
                        GIInfoType interface_type;
                        gsize size;

                        interface_info = g_type_info_get_interface(&ainfo);
                        g_assert(interface_info != NULL);

                        interface_type = g_base_info_get_type(interface_info);

                        if (interface_type == GI_INFO_TYPE_STRUCT) {
                            size = g_struct_info_get_size((GIStructInfo*)interface_info);
                        } else if (interface_type == GI_INFO_TYPE_UNION) {
                            size = g_union_info_get_size((GIUnionInfo*)interface_info);
                        } else {
                            failed = TRUE;
                        }

                        g_base_info_unref((GIBaseInfo*)interface_info);

                        if (!failed) {
                            in_arg_cvalues[c_arg_pos].v_pointer = g_slice_alloc0(size);
                            out_arg_cvalues[c_arg_pos].v_pointer = in_arg_cvalues[c_arg_pos].v_pointer;
                        }
                        break;
                    }
                default:
                    failed = TRUE;
                }
                if (failed)
                    gjs_throw(context, "Unsupported type %s for (out caller-allocates)", g_type_tag_to_string(type_tag));
            } else {
                out_arg_cvalues[c_arg_pos].v_pointer = NULL;
                in_arg_cvalues[c_arg_pos].v_pointer = &out_arg_cvalues[c_arg_pos];
            }
        } else {
            GArgument *in_value;
            GITypeInfo ainfo;
            ParamType param_type;

            g_arg_info_load_type(&arg_info, &ainfo);

            in_value = &in_arg_cvalues[c_arg_pos];

            param_type = function->param_types[gi_arg_pos];

            switch (param_type) {
            case PARAM_CALLBACK: {
                GICallableInfo *callable_info;
                GIScopeType scope = g_arg_info_get_scope(&arg_info);
                GjsCallbackTrampoline *trampoline;
                ffi_closure *closure;
                jsval value = js_argv[js_arg_pos];

                if (JSVAL_IS_NULL(value) && g_arg_info_may_be_null(&arg_info)) {
                    closure = NULL;
                    trampoline = NULL;
                } else {
                    if (!(JS_TypeOfValue(context, value) == JSTYPE_FUNCTION)) {
                        gjs_throw(context, "Error invoking %s.%s: Invalid callback given for argument %s",
                                  g_base_info_get_namespace( (GIBaseInfo*) function->info),
                                  g_base_info_get_name( (GIBaseInfo*) function->info),
                                  g_base_info_get_name( (GIBaseInfo*) &arg_info));
                        failed = TRUE;
                        break;
                    }

                    callable_info = (GICallableInfo*) g_type_info_get_interface(&ainfo);
                    trampoline = gjs_callback_trampoline_new(context,
                                                             value,
                                                             callable_info,
                                                             scope);
                    closure = trampoline->closure;
                    g_base_info_unref(callable_info);
                }

                gint destroy_pos = g_arg_info_get_destroy(&arg_info);
                gint closure_pos = g_arg_info_get_closure(&arg_info);
                if (destroy_pos >= 0) {
                    gint c_pos = is_method ? destroy_pos + 1 : destroy_pos;
                    g_assert (function->param_types[destroy_pos] == PARAM_SKIPPED);
                    in_arg_cvalues[c_pos].v_pointer = trampoline ? gjs_destroy_notify_callback : NULL;
                }
                if (closure_pos >= 0) {
                    gint c_pos = is_method ? closure_pos + 1 : closure_pos;
                    g_assert (function->param_types[closure_pos] == PARAM_SKIPPED);
                    in_arg_cvalues[c_pos].v_pointer = trampoline;
                }

                if (trampoline && scope != GI_SCOPE_TYPE_CALL) {
                    /* Add an extra reference that will be cleared when collecting
                       async calls, or when GDestroyNotify is called */
                    gjs_callback_trampoline_ref(trampoline);
                }
                in_value->v_pointer = closure;
                break;
            }
            case PARAM_SKIPPED:
                arg_removed = TRUE;
                break;
            case PARAM_ARRAY: {
                GIArgInfo array_length_arg;

                gint array_length_pos = g_type_info_get_array_length(&ainfo);
                gsize length;

                if (!gjs_value_to_explicit_array(context, js_argv[js_arg_pos], &arg_info,
                                                 in_value, &length)) {
                    failed = TRUE;
                    break;
                }

                g_callable_info_load_arg(function->info, array_length_pos, &array_length_arg);

                array_length_pos += is_method ? 1 : 0;
                if (!gjs_value_to_arg(context, INT_TO_JSVAL(length), &array_length_arg,
                                      in_arg_cvalues + array_length_pos)) {
                    failed = TRUE;
                    break;
                }
                /* Also handle the INOUT for the length here */
                if (direction == GI_DIRECTION_INOUT) {
                    if (in_value->v_pointer == NULL) { 
                        /* Special case where we were given JS null to
                         * also pass null for length, and not a
                         * pointer to an integer that derefs to 0.
                         */
                        in_arg_cvalues[array_length_pos].v_pointer = NULL;
                        out_arg_cvalues[array_length_pos].v_pointer = NULL;
                        inout_original_arg_cvalues[array_length_pos].v_pointer = NULL;
                    } else {
                        out_arg_cvalues[array_length_pos] = inout_original_arg_cvalues[array_length_pos] = *(in_arg_cvalues + array_length_pos);
                        in_arg_cvalues[array_length_pos].v_pointer = &out_arg_cvalues[array_length_pos];
                    }
                }
                break;
            }
            case PARAM_NORMAL:
                /* Ok, now just convert argument normally */
                g_assert_cmpuint(js_arg_pos, <, js_argc);
                if (!gjs_value_to_arg(context, js_argv[js_arg_pos], &arg_info,
                                      in_value)) {
                    failed = TRUE;
                    break;
                }
            }

            if (direction == GI_DIRECTION_INOUT && !arg_removed && !failed) {
                out_arg_cvalues[c_arg_pos] = inout_original_arg_cvalues[c_arg_pos] = in_arg_cvalues[c_arg_pos];
                in_arg_cvalues[c_arg_pos].v_pointer = &out_arg_cvalues[c_arg_pos];
            }

            if (failed) {
                /* Exit from the loop */
                break;
            }

            if (!failed && !arg_removed)
                ++js_arg_pos;
        }

        if (failed)
            break;

        processed_c_args++;
    }

    /* Did argument conversion fail?  In that case, skip invocation and jump to release
     * processing. */
    if (failed) {
        did_throw_gerror = FALSE;
        goto release;
    }

    if (can_throw_gerror) {
        g_assert_cmpuint(c_arg_pos, <, c_argc);
        in_arg_cvalues[c_arg_pos].v_pointer = &local_error;
        ffi_arg_pointers[c_arg_pos] = &(in_arg_cvalues[c_arg_pos]);
        c_arg_pos++;

        /* don't update processed_c_args as we deal with local_error
         * separately */
    }

    gjs_runtime_push_context(JS_GetRuntime(context), context);

    g_assert_cmpuint(c_arg_pos, ==, c_argc);
    g_assert_cmpuint(gi_arg_pos, ==, gi_argc);
    ffi_call(&(function->invoker.cif), function->invoker.native_address, &return_value, ffi_arg_pointers);

    gjs_runtime_pop_context(JS_GetRuntime(context));

    /* Return value and out arguments are valid only if invocation doesn't
     * return error. In arguments need to be released always.
     */
    if (can_throw_gerror) {
        did_throw_gerror = local_error != NULL;
    } else {
        did_throw_gerror = FALSE;
    }

    *js_rval = JSVAL_VOID;

    /* Only process return values if the function didn't throw */
    if (function->js_out_argc > 0 && !did_throw_gerror) {
        return_values = g_newa(jsval, function->js_out_argc);
        gjs_set_values(context, return_values, function->js_out_argc, JSVAL_VOID);
        gjs_root_value_locations(context, return_values, function->js_out_argc);

        if (return_tag != GI_TYPE_TAG_VOID) {
            GITransfer transfer = g_callable_info_get_caller_owns((GICallableInfo*) function->info);
            gboolean arg_failed;
            gint array_length_pos;

            g_assert_cmpuint(next_rval, <, function->js_out_argc);

            array_length_pos = g_type_info_get_array_length(&return_info);
            if (array_length_pos >= 0) {
                GIArgInfo array_length_arg;
                GITypeInfo arg_type_info;
                jsval length;

                g_callable_info_load_arg(function->info, array_length_pos, &array_length_arg);
                g_arg_info_load_type(&array_length_arg, &arg_type_info);
                array_length_pos += is_method ? 1 : 0;
                arg_failed = !gjs_value_from_g_argument(context, &length,
                                                        &arg_type_info,
                                                        &out_arg_cvalues[array_length_pos]);
                if (!arg_failed) {
                    arg_failed = !gjs_value_from_explicit_array(context,
                                                                &return_values[next_rval],
                                                                &return_info,
                                                                &return_value,
                                                                JSVAL_TO_INT(length));
                }
                if (!arg_failed &&
                    !gjs_g_argument_release_out_array(context,
                                                      transfer,
                                                      &return_info,
                                                      JSVAL_TO_INT(length),
                                                      &return_value))
                    failed = TRUE;
            } else {
                arg_failed = !gjs_value_from_g_argument(context, &return_values[next_rval],
                                                        &return_info, &return_value);
                /* Free GArgument, the jsval should have ref'd or copied it */
                if (!arg_failed &&
                    !gjs_g_argument_release(context,
                                            transfer,
                                            &return_info,
                                            &return_value))
                    failed = TRUE;
            }
            if (arg_failed)
                failed = TRUE;

            ++next_rval;
        }
    }

release:
    /* We walk over all args, release in args (if allocated) and convert
     * all out args to JS
     */
    c_arg_pos = is_method ? 1 : 0;
    postinvoke_release_failed = FALSE;
    for (gi_arg_pos = 0; gi_arg_pos < gi_argc && c_arg_pos < processed_c_args; gi_arg_pos++, c_arg_pos++) {
        GIDirection direction;
        GIArgInfo arg_info;
        GITypeInfo arg_type_info;
        ParamType param_type;

        g_callable_info_load_arg( (GICallableInfo*) function->info, gi_arg_pos, &arg_info);
        direction = g_arg_info_get_direction(&arg_info);

        g_arg_info_load_type(&arg_info, &arg_type_info);
        param_type = function->param_types[gi_arg_pos];

        if (direction == GI_DIRECTION_IN || direction == GI_DIRECTION_INOUT) {
            GArgument *arg;
            GITransfer transfer;

            if (direction == GI_DIRECTION_IN) {
                arg = &in_arg_cvalues[c_arg_pos];
                transfer = g_arg_info_get_ownership_transfer(&arg_info);
            } else {
                arg = &inout_original_arg_cvalues[c_arg_pos];
                /* For inout, transfer refers to what we get back from the function; for
                 * the temporary C value we allocated, clearly we're responsible for
                 * freeing it.
                 */
                transfer = GI_TRANSFER_NOTHING;
            }
            if (param_type == PARAM_CALLBACK) {
                ffi_closure *closure = arg->v_pointer;
                if (closure) {
                    GjsCallbackTrampoline *trampoline = closure->user_data;
                    /* CallbackTrampolines are refcounted because for notified/async closures
                       it is possible to destroy it while in call, and therefore we cannot check
                       its scope at this point */
                    gjs_callback_trampoline_unref(trampoline);
                    arg->v_pointer = NULL;
                }
            } else if (param_type == PARAM_ARRAY) {
                gsize length;
                GIArgInfo array_length_arg;
                GITypeInfo array_length_type;
                gint array_length_pos = g_type_info_get_array_length(&arg_type_info);

                g_assert(array_length_pos >= 0);

                g_callable_info_load_arg(function->info, array_length_pos, &array_length_arg);
                g_arg_info_load_type(&array_length_arg, &array_length_type);

                array_length_pos += is_method ? 1 : 0;

                length = get_length_from_arg(in_arg_cvalues + array_length_pos,
                                             g_type_info_get_tag(&array_length_type));

                if (!gjs_g_argument_release_in_array(context,
                                                     transfer,
                                                     &arg_type_info,
                                                     length,
                                                     arg)) {
                    postinvoke_release_failed = TRUE;
                }
            } else if (param_type == PARAM_NORMAL) {
                if (!gjs_g_argument_release_in_arg(context,
                                                   transfer,
                                                   &arg_type_info,
                                                   arg)) {
                    postinvoke_release_failed = TRUE;
                }
            }
        }

        /* Don't free out arguments if function threw an exception or we failed
         * earlier - note "postinvoke_release_failed" is separate from "failed".  We
         * sync them up after this loop.
         */
        if (did_throw_gerror || failed)
            continue;

        if ((direction == GI_DIRECTION_OUT || direction == GI_DIRECTION_INOUT) && param_type != PARAM_SKIPPED) {
            GArgument *arg;
            gboolean arg_failed;
            gint array_length_pos;
            jsval array_length;
            GITransfer transfer;

            g_assert(next_rval < function->js_out_argc);

            arg = &out_arg_cvalues[c_arg_pos];

            array_length_pos = g_type_info_get_array_length(&arg_type_info);
            if (array_length_pos >= 0) {
                GIArgInfo array_length_arg;
                GITypeInfo array_length_type_info;

                g_callable_info_load_arg(function->info, array_length_pos, &array_length_arg);
                g_arg_info_load_type(&array_length_arg, &array_length_type_info);
                array_length_pos += is_method ? 1 : 0;
                arg_failed = !gjs_value_from_g_argument(context, &array_length,
                                                        &array_length_type_info,
                                                        &out_arg_cvalues[array_length_pos]);
                if (!arg_failed) {
                    arg_failed = !gjs_value_from_explicit_array(context,
                                                                &return_values[next_rval],
                                                                &arg_type_info,
                                                                arg,
                                                                JSVAL_TO_INT(array_length));
                }
            } else {
                arg_failed = !gjs_value_from_g_argument(context,
                                                        &return_values[next_rval],
                                                        &arg_type_info,
                                                        arg);
            }

            if (arg_failed)
                postinvoke_release_failed = TRUE;

            /* For caller-allocates, what happens here is we allocate
             * a structure above, then gjs_value_from_g_argument calls
             * g_boxed_copy on it, and takes ownership of that.  So
             * here we release the memory allocated above.  It would be
             * better to special case this and directly hand JS the boxed
             * object and tell gjs_boxed it owns the memory, but for now
             * this works OK.  We could also alloca() the structure instead
             * of slice allocating.
             */
            if (g_arg_info_is_caller_allocates(&arg_info)) {
                GITypeTag type_tag;
                GIBaseInfo* interface_info;
                GIInfoType interface_type;
                gsize size;

                type_tag = g_type_info_get_tag(&arg_type_info);
                g_assert(type_tag == GI_TYPE_TAG_INTERFACE);
                interface_info = g_type_info_get_interface(&arg_type_info);
                interface_type = g_base_info_get_type(interface_info);
                if (interface_type == GI_INFO_TYPE_STRUCT) {
                    size = g_struct_info_get_size((GIStructInfo*)interface_info);
                } else if (interface_type == GI_INFO_TYPE_UNION) {
                    size = g_union_info_get_size((GIUnionInfo*)interface_info);
                } else {
                    g_assert_not_reached();
                }

                g_slice_free1(size, out_arg_cvalues[c_arg_pos].v_pointer);
                g_base_info_unref((GIBaseInfo*)interface_info);
            }

            /* Free GArgument, the jsval should have ref'd or copied it */
            transfer = g_arg_info_get_ownership_transfer(&arg_info);
            if (!arg_failed) {
                if (array_length_pos >= 0) {
                    gjs_g_argument_release_out_array(context,
                                                     transfer,
                                                     &arg_type_info,
                                                     JSVAL_TO_INT(array_length),
                                                     arg);
                } else {
                    gjs_g_argument_release(context,
                                           transfer,
                                           &arg_type_info,
                                           arg);
                }
            }

            ++next_rval;
        }
    }

    if (postinvoke_release_failed)
        failed = TRUE;

    g_assert(failed || did_throw_gerror || next_rval == (guint8)function->js_out_argc);
    g_assert_cmpuint(c_arg_pos, ==, processed_c_args);

    if (function->js_out_argc > 0 && (!failed && !did_throw_gerror)) {
        /* if we have 1 return value or out arg, return that item
         * on its own, otherwise return a JavaScript array with
         * [return value, out arg 1, out arg 2, ...]
         */
        if (function->js_out_argc == 1) {
            *js_rval = return_values[0];
        } else {
            JSObject *array;
            array = JS_NewArrayObject(context,
                                      function->js_out_argc,
                                      return_values);
            if (array == NULL) {
                failed = TRUE;
            } else {
                *js_rval = OBJECT_TO_JSVAL(array);
            }
        }

        gjs_unroot_value_locations(context, return_values, function->js_out_argc);
    }

    if (!failed && did_throw_gerror) {
        gjs_throw(context, "Error invoking %s.%s: %s",
                  g_base_info_get_namespace( (GIBaseInfo*) function->info),
                  g_base_info_get_name( (GIBaseInfo*) function->info),
                  local_error->message);
        g_error_free(local_error);
        return JS_FALSE;
    } else if (failed) {
        return JS_FALSE;
    } else {
        return JS_TRUE;
    }
}

static JSBool
function_call(JSContext *context,
              uintN      js_argc,
              jsval     *vp)
{
    jsval *js_argv = JS_ARGV(context, vp);
    JSObject *object = JS_THIS_OBJECT(context, vp);
    JSObject *callee = JSVAL_TO_OBJECT(JS_CALLEE(context, vp));
    JSBool success;
    Function *priv;
    jsval retval;

    priv = priv_from_js(context, callee);
    gjs_debug_marshal(GJS_DEBUG_GFUNCTION, "Call callee %p priv %p this obj %p %s", callee, priv,
                      obj, JS_GetTypeName(context,
                                          JS_TypeOfValue(context, OBJECT_TO_JSVAL(object))));

    if (priv == NULL)
        return JS_TRUE; /* we are the prototype, or have the wrong class */


    success = gjs_invoke_c_function(context, priv, object, js_argc, js_argv, &retval);
    if (success)
        JS_SET_RVAL(context, vp, retval);

    return success;
}

GJS_NATIVE_CONSTRUCTOR_DECLARE(function)
{
    GJS_NATIVE_CONSTRUCTOR_VARIABLES(function)
    Function *priv;

    GJS_NATIVE_CONSTRUCTOR_PRELUDE(name);

    priv = g_slice_new0(Function);

    GJS_INC_COUNTER(function);

    g_assert(priv_from_js(context, object) == NULL);
    JS_SetPrivate(context, object, priv);

    gjs_debug_lifecycle(GJS_DEBUG_GFUNCTION,
                        "function constructor, obj %p priv %p", object, priv);

    GJS_NATIVE_CONSTRUCTOR_FINISH(name);

    return JS_TRUE;
}

/* Does not actually free storage for structure, just
 * reverses init_cached_function_data
 */
static void
uninit_cached_function_data (Function *function)
{
    if (function->info)
        g_base_info_unref( (GIBaseInfo*) function->info);
    if (function->param_types)
        g_free(function->param_types);

    g_function_invoker_destroy(&function->invoker);
}

static void
function_finalize(JSContext *context,
                  JSObject  *obj)
{
    Function *priv;

    priv = priv_from_js(context, obj);
    gjs_debug_lifecycle(GJS_DEBUG_GFUNCTION,
                        "finalize, obj %p priv %p", obj, priv);
    if (priv == NULL)
        return; /* we are the prototype, not a real instance, so constructor never called */

    uninit_cached_function_data(priv);

    GJS_DEC_COUNTER(function);
    g_slice_free(Function, priv);
}

static JSBool
function_to_string (JSContext *context,
                    guint      argc,
                    jsval     *vp)
{
    Function *priv;
    gchar *string;
    gboolean free;
    JSObject *self;
    jsval retval;
    JSBool ret = JS_FALSE;

    self = JS_THIS_OBJECT(context, vp);
    if (!self) {
        gjs_throw(context, "this cannot be null");
        return JS_FALSE;
    }

    priv = priv_from_js (context, self);

    if (priv == NULL) {
        string = "function () {\n}";
        free = FALSE;
    } else {
        string = g_strdup_printf("function %s(){\n\t/* proxy for native symbol %s(); */\n}",
                                 g_base_info_get_name ((GIBaseInfo *) priv->info),
                                 g_function_info_get_symbol (priv->info));
        free = TRUE;
    }

    if (gjs_string_from_utf8(context, string, -1, &retval)) {
        JS_SET_RVAL(context, vp, retval);
        ret = JS_TRUE;
    }

    if (free)
        g_free(string);
    return ret;
}

/* The bizarre thing about this vtable is that it applies to both
 * instances of the object, and to the prototype that instances of the
 * class have.
 *
 * Also, there's a constructor field in here, but as far as I can
 * tell, it would only be used if no constructor were provided to
 * JS_InitClass. The constructor from JS_InitClass is not applied to
 * the prototype unless JSCLASS_CONSTRUCT_PROTOTYPE is in flags.
 */
static struct JSClass gjs_function_class = {
    "GIRepositoryFunction", /* means "new GIRepositoryFunction()" works */
    JSCLASS_HAS_PRIVATE |
    JSCLASS_NEW_RESOLVE |
    JSCLASS_NEW_RESOLVE_GETS_START,
    JS_PropertyStub,
    JS_PropertyStub,
    JS_PropertyStub,
    JS_StrictPropertyStub,
    JS_EnumerateStub,
    (JSResolveOp) function_new_resolve, /* needs cast since it's the new resolve signature */
    JS_ConvertStub,
    function_finalize,
    NULL,
    NULL,
    function_call,
    NULL, NULL, NULL, NULL, NULL
};

static JSPropertySpec gjs_function_proto_props[] = {
    { NULL }
};

/* The original Function.prototype.toString complains when
   given a GIRepository function as an argument */
static JSFunctionSpec gjs_function_proto_funcs[] = {
    JS_FN("toString", function_to_string, 0, 0),
    JS_FS_END
};

static gboolean
init_cached_function_data (JSContext      *context,
                           Function       *function,
                           GIFunctionInfo *info)
{
    guint8 i, n_args, array_length_pos;
    GError *error = NULL;
    GITypeInfo return_type;

    if (!g_function_info_prep_invoker(info, &(function->invoker), &error)) {
        gjs_throw_g_error(context, error);
        return FALSE;
    }

    g_callable_info_load_return_type((GICallableInfo*)info, &return_type);
    if (g_type_info_get_tag(&return_type) != GI_TYPE_TAG_VOID)
        function->js_out_argc += 1;

    n_args = g_callable_info_get_n_args((GICallableInfo*) info);
    function->param_types = g_new0(ParamType, n_args);

    array_length_pos = g_type_info_get_array_length(&return_type);
    if (array_length_pos >= 0 && array_length_pos < n_args)
        function->param_types[array_length_pos] = PARAM_SKIPPED;

    for (i = 0; i < n_args; i++) {
        GIDirection direction;
        GIArgInfo arg_info;
        GITypeInfo type_info;
        guint8 destroy = -1;
        guint8 closure = -1;
        GITypeTag type_tag;

        if (function->param_types[i] == PARAM_SKIPPED)
            continue;

        g_callable_info_load_arg((GICallableInfo*) info, i, &arg_info);
        g_arg_info_load_type(&arg_info, &type_info);

        direction = g_arg_info_get_direction(&arg_info);
        type_tag = g_type_info_get_tag(&type_info);

        if (type_tag == GI_TYPE_TAG_INTERFACE) {
            GIBaseInfo* interface_info;
            GIInfoType interface_type;

            interface_info = g_type_info_get_interface(&type_info);
            interface_type = g_base_info_get_type(interface_info);
            if (interface_type == GI_INFO_TYPE_CALLBACK) {
                if (strcmp(g_base_info_get_name(interface_info), "DestroyNotify") == 0 &&
                    strcmp(g_base_info_get_namespace(interface_info), "GLib") == 0) {
                    /* Skip GDestroyNotify if they appear before the respective callback */
                    function->param_types[i] = PARAM_SKIPPED;
                } else {
                    function->param_types[i] = PARAM_CALLBACK;
                    function->expected_js_argc += 1;

                    destroy = g_arg_info_get_destroy(&arg_info);
                    closure = g_arg_info_get_closure(&arg_info);

                    if (destroy >= 0 && destroy < n_args)
                        function->param_types[destroy] = PARAM_SKIPPED;

                    if (closure >= 0 && closure < n_args)
                        function->param_types[closure] = PARAM_SKIPPED;

                    if (destroy >= 0 && closure < 0) {
                        gjs_throw(context, "Function %s.%s has a GDestroyNotify but no user_data, not supported",
                                  g_base_info_get_namespace( (GIBaseInfo*) info),
                                  g_base_info_get_name( (GIBaseInfo*) info));
                        return JS_FALSE;
                    }
                }
            }
            g_base_info_unref(interface_info);
        } else if (type_tag == GI_TYPE_TAG_ARRAY) {
            if (g_type_info_get_array_type(&type_info) == GI_ARRAY_TYPE_C) {
                array_length_pos = g_type_info_get_array_length(&type_info);

                if (array_length_pos >= 0 && array_length_pos < n_args) {
                    GIArgInfo length_arg_info;

                    g_callable_info_load_arg((GICallableInfo*) info, array_length_pos, &length_arg_info);
                    if (g_arg_info_get_direction(&length_arg_info) != direction) {
                        gjs_throw(context, "Function %s.%s has an array with different-direction length arg, not supported",
                                  g_base_info_get_namespace( (GIBaseInfo*) info),
                                  g_base_info_get_name( (GIBaseInfo*) info));
                        return JS_FALSE;
                    }

                    function->param_types[array_length_pos] = PARAM_SKIPPED;
                    function->param_types[i] = PARAM_ARRAY;

                    if (array_length_pos < i) {
                        /* we already collected array_length_pos, remove it */
                        function->expected_js_argc -= 1;
                        if (direction == GI_DIRECTION_OUT || direction == GI_DIRECTION_INOUT)
                            function->js_out_argc--;
                    }
                }
            }
        }

        if (function->param_types[i] == PARAM_NORMAL ||
            function->param_types[i] == PARAM_ARRAY) {
            if (direction == GI_DIRECTION_IN || direction == GI_DIRECTION_INOUT)
                function->expected_js_argc += 1;
            if (direction == GI_DIRECTION_OUT || direction == GI_DIRECTION_INOUT)
                function->js_out_argc += 1;
        }
    }

    function->info = info;

    g_base_info_ref((GIBaseInfo*) function->info);

    return JS_TRUE;
}

static JSObject*
function_new(JSContext      *context,
             GIFunctionInfo *info)
{
    JSObject *function;
    JSObject *global;
    Function *priv;

    /* put constructor for GIRepositoryFunction() in the global namespace */
    global = gjs_get_import_global(context);

    if (!gjs_object_has_property(context, global, gjs_function_class.name)) {
        JSObject *prototype;
        JSObject *parent_proto;
        jsval native_function;

        JS_GetProperty(context, global, "Function", &native_function);
        /* We take advantage from that fact that Function.__proto__ is Function.prototype */
        parent_proto = JS_GetPrototype(context, JSVAL_TO_OBJECT(native_function));

        prototype = JS_InitClass(context, global,
                                 /* parent prototype JSObject* for
                                  * prototype; NULL for
                                  * Object.prototype
                                  */
                                 parent_proto,
                                 &gjs_function_class,
                                 /* constructor for instances (NULL for
                                  * none - just name the prototype like
                                  * Math - rarely correct)
                                  */
                                 gjs_function_constructor,
                                 /* number of constructor args */
                                 0,
                                 /* props of prototype */
                                 &gjs_function_proto_props[0],
                                 /* funcs of prototype */
                                 &gjs_function_proto_funcs[0],
                                 /* props of constructor, MyConstructor.myprop */
                                 NULL,
                                 /* funcs of constructor, MyConstructor.myfunc() */
                                 NULL);
        if (prototype == NULL)
            gjs_fatal("Can't init class %s", gjs_function_class.name);

        g_assert(gjs_object_has_property(context, global, gjs_function_class.name));

        gjs_debug(GJS_DEBUG_GFUNCTION, "Initialized class %s prototype %p",
                  gjs_function_class.name, prototype);
    }

    function = JS_ConstructObject(context, &gjs_function_class, NULL, global);
    if (function == NULL) {
        gjs_debug(GJS_DEBUG_GFUNCTION, "Failed to construct function");
        return NULL;
    }

    priv = priv_from_js(context, function);
    if (!init_cached_function_data(context, priv, info))
      return NULL;

    return function;
}

JSObject*
gjs_define_function(JSContext      *context,
                    JSObject       *in_object,
                    GIFunctionInfo *info)
{
    JSObject *function;

    JS_BeginRequest(context);

    function = function_new(context, info);
    if (function == NULL) {
        gjs_move_exception(context, context);

        JS_EndRequest(context);
        return NULL;
    }

    if (!JS_DefineProperty(context, in_object,
                           g_base_info_get_name( (GIBaseInfo*) info),
                           OBJECT_TO_JSVAL(function),
                           NULL, NULL,
                           GJS_MODULE_PROP_FLAGS)) {
        gjs_debug(GJS_DEBUG_GFUNCTION, "Failed to define function");

        JS_EndRequest(context);
        return NULL;
    }

    JS_EndRequest(context);
    return function;
}


JSBool
gjs_invoke_c_function_uncached (JSContext      *context,
                                GIFunctionInfo *info,
                                JSObject       *obj,
                                uintN           argc,
                                jsval          *argv,
                                jsval          *rval)
{
  Function function;
  JSBool result;

  memset (&function, 0, sizeof (Function));
  if (!init_cached_function_data (context, &function, info))
    return JS_FALSE;

  result = gjs_invoke_c_function (context, &function, obj, argc, argv, rval);
  uninit_cached_function_data (&function);
  return result;
}
