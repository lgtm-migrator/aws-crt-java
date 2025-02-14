/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "http_request_utils.h"

#include "crt.h"
#include "java_class_ids.h"

#include <aws/common/byte_order.h>
#include <aws/http/http.h>
#include <aws/http/request_response.h>
#include <aws/io/stream.h>

#if _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

struct aws_http_request_body_stream_impl {
    struct aws_input_stream base;
    struct aws_allocator *allocator;
    JavaVM *jvm;
    jobject http_request_body_stream;
    bool body_done;
    bool is_valid;
};

static int s_aws_input_stream_seek(struct aws_input_stream *stream, int64_t offset, enum aws_stream_seek_basis basis) {
    struct aws_http_request_body_stream_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_http_request_body_stream_impl, base);

    if (!impl->is_valid) {
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_BODY_STREAM);
    }

    int result = AWS_OP_SUCCESS;
    if (impl->http_request_body_stream != NULL) {
        if (basis != AWS_SSB_BEGIN || offset != 0) {
            return AWS_OP_ERR;
        }

        /********** JNI ENV ACQUIRE **********/
        JNIEnv *env = aws_jni_acquire_thread_env(impl->jvm);
        if (env == NULL) {
            /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
            return AWS_OP_ERR;
        }

        if (!(*env)->CallBooleanMethod(
                env, impl->http_request_body_stream, http_request_body_stream_properties.reset_position)) {
            result = aws_raise_error(AWS_ERROR_HTTP_CALLBACK_FAILURE);
        }

        if (aws_jni_check_and_clear_exception(env)) {
            result = aws_raise_error(AWS_ERROR_HTTP_CALLBACK_FAILURE);
        }

        aws_jni_release_thread_env(impl->jvm, env);
        /********** JNI ENV RELEASE **********/
    }

    if (result == AWS_OP_SUCCESS) {
        impl->body_done = false;
    }

    return result;
}

static int s_aws_input_stream_read(struct aws_input_stream *stream, struct aws_byte_buf *dest) {
    struct aws_http_request_body_stream_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_http_request_body_stream_impl, base);

    if (!impl->is_valid) {
        return aws_raise_error(AWS_ERROR_HTTP_INVALID_BODY_STREAM);
    }

    if (impl->http_request_body_stream == NULL) {
        impl->body_done = true;
        return AWS_OP_SUCCESS;
    }

    if (impl->body_done) {
        return AWS_OP_SUCCESS;
    }

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(impl->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return AWS_OP_ERR;
    }

    size_t out_remaining = dest->capacity - dest->len;

    jobject direct_buffer = aws_jni_direct_byte_buffer_from_raw_ptr(env, dest->buffer + dest->len, out_remaining);

    impl->body_done = (*env)->CallBooleanMethod(
        env, impl->http_request_body_stream, http_request_body_stream_properties.send_outgoing_body, direct_buffer);

    int result = AWS_OP_SUCCESS;
    if (aws_jni_check_and_clear_exception(env)) {
        result = aws_raise_error(AWS_ERROR_HTTP_CALLBACK_FAILURE);
    } else {
        size_t amt_written = aws_jni_byte_buffer_get_position(env, direct_buffer);
        dest->len += amt_written;
    }

    (*env)->DeleteLocalRef(env, direct_buffer);

    aws_jni_release_thread_env(impl->jvm, env);
    /********** JNI ENV RELEASE **********/

    return result;
}

static int s_aws_input_stream_get_status(struct aws_input_stream *stream, struct aws_stream_status *status) {
    struct aws_http_request_body_stream_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_http_request_body_stream_impl, base);

    status->is_end_of_stream = impl->body_done;
    status->is_valid = impl->is_valid;

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_get_length(struct aws_input_stream *stream, int64_t *length) {
    AWS_FATAL_ASSERT(length && "NULL length out param passed to JNI aws_input_stream_get_length");
    struct aws_http_request_body_stream_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_http_request_body_stream_impl, base);

    if (impl->http_request_body_stream != NULL) {

        /********** JNI ENV ACQUIRE **********/
        JNIEnv *env = aws_jni_acquire_thread_env(impl->jvm);
        if (env == NULL) {
            /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
            return AWS_OP_ERR;
        }

        *length =
            (*env)->CallLongMethod(env, impl->http_request_body_stream, http_request_body_stream_properties.get_length);

        int result = AWS_OP_SUCCESS;
        if (aws_jni_check_and_clear_exception(env)) {
            result = aws_raise_error(AWS_ERROR_HTTP_CALLBACK_FAILURE);
        }

        aws_jni_release_thread_env(impl->jvm, env);
        /********** JNI ENV RELEASE **********/

        return result;
    }

    return AWS_OP_ERR;
}

static void s_aws_input_stream_destroy(struct aws_http_request_body_stream_impl *impl) {

    /********** JNI ENV ACQUIRE **********/
    JNIEnv *env = aws_jni_acquire_thread_env(impl->jvm);
    if (env == NULL) {
        /* If we can't get an environment, then the JVM is probably shutting down.  Don't crash. */
        return;
    }

    if (impl->http_request_body_stream != NULL) {
        (*env)->DeleteGlobalRef(env, impl->http_request_body_stream);
    }

    aws_jni_release_thread_env(impl->jvm, env);
    /********** JNI ENV RELEASE **********/

    aws_mem_release(impl->allocator, impl);
}

static struct aws_input_stream_vtable s_aws_input_stream_vtable = {
    .seek = s_aws_input_stream_seek,
    .read = s_aws_input_stream_read,
    .get_status = s_aws_input_stream_get_status,
    .get_length = s_aws_input_stream_get_length,
};

struct aws_input_stream *aws_input_stream_new_from_java_http_request_body_stream(
    struct aws_allocator *allocator,
    JNIEnv *env,
    jobject http_request_body_stream) {
    struct aws_http_request_body_stream_impl *impl =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_http_request_body_stream_impl));

    impl->allocator = allocator;
    impl->base.vtable = &s_aws_input_stream_vtable;
    aws_ref_count_init(&impl->base.ref_count, impl, (aws_simple_completion_callback *)s_aws_input_stream_destroy);

    jint jvmresult = (*env)->GetJavaVM(env, &impl->jvm);
    AWS_FATAL_ASSERT(jvmresult == 0);

    impl->is_valid = true;
    if (http_request_body_stream != NULL) {
        impl->http_request_body_stream = (*env)->NewGlobalRef(env, http_request_body_stream);
        if (impl->http_request_body_stream == NULL) {
            goto on_error;
        }
    } else {
        impl->body_done = true;
    }

    return &impl->base;

on_error:

    aws_input_stream_release(&impl->base);

    return NULL;
}

static inline int s_marshal_http_header_to_buffer(
    struct aws_byte_buf *buf,
    const struct aws_byte_cursor *name,
    const struct aws_byte_cursor *value) {
    if (aws_byte_buf_reserve_relative(buf, sizeof(int) + sizeof(int) + name->len + value->len)) {
        return AWS_OP_ERR;
    }

    aws_byte_buf_write_be32(buf, (uint32_t)name->len);
    aws_byte_buf_write_from_whole_cursor(buf, *name);
    aws_byte_buf_write_be32(buf, (uint32_t)value->len);
    aws_byte_buf_write_from_whole_cursor(buf, *value);
    return AWS_OP_SUCCESS;
}

int aws_marshal_http_headers_to_dynamic_buffer(
    struct aws_byte_buf *buf,
    const struct aws_http_header *header_array,
    size_t num_headers) {
    for (size_t i = 0; i < num_headers; ++i) {
        if (s_marshal_http_header_to_buffer(buf, &header_array[i].name, &header_array[i].value)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}
/**
 * Unmarshal the request from java.
 *
 * Version is as int: [4-bytes BE]
 *
 * Each string field is: [4-bytes BE] [variable length bytes specified
 *          by the previous field]
 *
 * Each request is like: [version][method][path][header name-value
 *          pairs]
 *
 * s_unmarshal_http_request_to_get_version to get the version field, which is a 4 byte int.
 * s_unmarshal_http_request_without_version to get the whole request without version field.
 */
static inline enum aws_http_version s_unmarshal_http_request_to_get_version(struct aws_byte_cursor *request_blob) {
    uint32_t version = 0;
    if (!aws_byte_cursor_read_be32(request_blob, &version)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    return version;
}

static inline int s_unmarshal_http_request_without_version(
    struct aws_http_message *message,
    struct aws_byte_cursor *request_blob) {
    uint32_t field_len = 0;
    if (aws_http_message_get_protocol_version(message) != AWS_HTTP_VERSION_2) {
        /* HTTP/1 puts method and path first, but those are empty in HTTP/2 */
        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor method = aws_byte_cursor_advance(request_blob, field_len);

        int result = aws_http_message_set_request_method(message, method);
        if (result != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor path = aws_byte_cursor_advance(request_blob, field_len);

        result = aws_http_message_set_request_path(message, path);
        if (result != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }
    } else {
        /* Read empty method and path from the marshalled request */
        if (!aws_byte_cursor_read_be32(request_blob, &field_len) || field_len) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
        if (!aws_byte_cursor_read_be32(request_blob, &field_len) || field_len) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }
    while (request_blob->len) {
        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor header_name = aws_byte_cursor_advance(request_blob, field_len);

        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor header_value = aws_byte_cursor_advance(request_blob, field_len);

        struct aws_http_header header = {
            .name = header_name,
            .value = header_value,
        };

        aws_http_message_add_header(message, header);
    }

    return AWS_OP_SUCCESS;
}

static inline int s_unmarshal_http_headers(struct aws_http_headers *headers, struct aws_byte_cursor *request_blob) {
    uint32_t field_len = 0;
    while (request_blob->len) {
        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor header_name = aws_byte_cursor_advance(request_blob, field_len);

        if (!aws_byte_cursor_read_be32(request_blob, &field_len)) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        struct aws_byte_cursor header_value = aws_byte_cursor_advance(request_blob, field_len);

        struct aws_http_header header = {
            .name = header_name,
            .value = header_value,
        };

        aws_http_headers_add_header(headers, &header);
    }
    return AWS_OP_SUCCESS;
}

int aws_apply_java_http_request_changes_to_native_request(
    JNIEnv *env,
    jbyteArray marshalled_request,
    jobject jni_body_stream,
    struct aws_http_message *message) {

    /* come back to this when we decide we need to. */
    (void)jni_body_stream;

    struct aws_http_headers *headers = aws_http_message_get_headers(message);
    aws_http_headers_clear(headers);
    int result = AWS_OP_SUCCESS;

    const size_t marshalled_request_length = (*env)->GetArrayLength(env, marshalled_request);

    uint8_t *marshalled_request_data = (*env)->GetPrimitiveArrayCritical(env, marshalled_request, NULL);
    struct aws_byte_cursor marshalled_cur =
        aws_byte_cursor_from_array((uint8_t *)marshalled_request_data, marshalled_request_length);

    enum aws_http_version version = s_unmarshal_http_request_to_get_version(&marshalled_cur);
    if (version != aws_http_message_get_protocol_version(message)) {
        result = aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    } else {
        result = s_unmarshal_http_request_without_version(message, &marshalled_cur);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, marshalled_request, marshalled_request_data, 0);

    if (result) {
        aws_jni_throw_runtime_exception(
            env, "HttpRequest.applyChangesToNativeRequest: %s\n", aws_error_debug_str(aws_last_error()));
        return result;
    }

    if (jni_body_stream) {
        struct aws_input_stream *body_stream =
            aws_input_stream_new_from_java_http_request_body_stream(aws_jni_get_allocator(), env, jni_body_stream);

        aws_http_message_set_body_stream(message, body_stream);
        /* request controls the lifetime of body stream fully */
        aws_input_stream_release(body_stream);
    }

    return result;
}

struct aws_http_message *aws_http_request_new_from_java_http_request(
    JNIEnv *env,
    jbyteArray marshalled_request,
    jobject jni_body_stream) {
    const char *exception_message = NULL;
    const size_t marshalled_request_length = (*env)->GetArrayLength(env, marshalled_request);

    jbyte *marshalled_request_data = (*env)->GetPrimitiveArrayCritical(env, marshalled_request, NULL);
    struct aws_byte_cursor marshalled_cur =
        aws_byte_cursor_from_array((uint8_t *)marshalled_request_data, marshalled_request_length);
    enum aws_http_version version = s_unmarshal_http_request_to_get_version(&marshalled_cur);
    struct aws_http_message *request = version == AWS_HTTP_VERSION_2
                                           ? aws_http2_message_new_request(aws_jni_get_allocator())
                                           : aws_http_message_new_request(aws_jni_get_allocator());

    int result = AWS_OP_SUCCESS;
    if (version != aws_http_message_get_protocol_version(request)) {
        result = aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    } else {
        result = s_unmarshal_http_request_without_version(request, &marshalled_cur);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, marshalled_request, marshalled_request_data, 0);

    if (result) {
        exception_message = "aws_http_request_new_from_java_http_request: Invalid marshalled request data.";
        goto on_error;
    }

    if (jni_body_stream != NULL) {
        struct aws_input_stream *body_stream =
            aws_input_stream_new_from_java_http_request_body_stream(aws_jni_get_allocator(), env, jni_body_stream);
        if (body_stream == NULL) {
            exception_message = "aws_fill_out_request: Error building body stream";
            goto on_error;
        }

        aws_http_message_set_body_stream(request, body_stream);
        /* request controls the lifetime of body stream fully */
        aws_input_stream_release(body_stream);
    }

    return request;

on_error:
    if (exception_message) {
        aws_jni_throw_runtime_exception(env, exception_message);
    }

    /* Don't need to destroy input stream since it's the last thing created */
    aws_http_message_destroy(request);

    return NULL;
}

struct aws_http_headers *aws_http_headers_new_from_java_http_headers(JNIEnv *env, jbyteArray marshalled_headers) {
    struct aws_http_headers *headers = aws_http_headers_new(aws_jni_get_allocator());
    if (headers == NULL) {
        aws_jni_throw_runtime_exception(env, "aws_http_headers_new_from_java_http_headers: Unable to allocate headers");
        return NULL;
    }
    const size_t marshalled_headers_length = (*env)->GetArrayLength(env, marshalled_headers);

    jbyte *marshalled_headers_data = (*env)->GetPrimitiveArrayCritical(env, marshalled_headers, NULL);
    struct aws_byte_cursor marshalled_cur =
        aws_byte_cursor_from_array((uint8_t *)marshalled_headers_data, marshalled_headers_length);
    int result = s_unmarshal_http_headers(headers, &marshalled_cur);
    (*env)->ReleasePrimitiveArrayCritical(env, marshalled_headers, marshalled_headers_data, 0);

    if (result) {
        aws_jni_throw_runtime_exception(
            env, "aws_http_headers_new_from_java_http_headers: Invalid marshalled headers data.");
        goto on_error;
    }

    return headers;

on_error:
    aws_http_headers_release(headers);
    return NULL;
}

static inline int s_marshall_http_request(const struct aws_http_message *message, struct aws_byte_buf *request_buf) {
    struct aws_byte_cursor method;
    AWS_ZERO_STRUCT(method);

    AWS_FATAL_ASSERT(!aws_http_message_get_request_method(message, &method));

    struct aws_byte_cursor path;
    AWS_ZERO_STRUCT(path);

    AWS_FATAL_ASSERT(!aws_http_message_get_request_path(message, &path));

    if (aws_byte_buf_reserve_relative(request_buf, sizeof(int) + sizeof(int) + sizeof(int) + method.len + path.len)) {
        return AWS_OP_ERR;
    }

    aws_byte_buf_write_be32(request_buf, (uint32_t)aws_http_message_get_protocol_version(message));
    aws_byte_buf_write_be32(request_buf, (uint32_t)method.len);
    aws_byte_buf_write_from_whole_cursor(request_buf, method);
    aws_byte_buf_write_be32(request_buf, (uint32_t)path.len);
    aws_byte_buf_write_from_whole_cursor(request_buf, path);

    const struct aws_http_headers *headers = aws_http_message_get_const_headers(message);
    AWS_FATAL_ASSERT(headers);
    size_t header_count = aws_http_message_get_header_count(message);
    for (size_t i = 0; i < header_count; ++i) {
        struct aws_http_header header;
        AWS_ZERO_STRUCT(header);

        AWS_FATAL_ASSERT(!aws_http_headers_get_index(headers, i, &header));
        if (s_marshal_http_header_to_buffer(request_buf, &header.name, &header.value)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

jobject aws_java_http_request_from_native(JNIEnv *env, struct aws_http_message *message, jobject request_body_stream) {
    jobject jni_request_blob = NULL;
    jobject j_request = NULL;
    struct aws_byte_buf marshaling_buf;

    if (aws_byte_buf_init(&marshaling_buf, aws_jni_get_allocator(), 1024)) {
        aws_jni_throw_runtime_exception(env, "aws_java_http_request_from_native: allocation failed");
        return NULL;
    }

    if (s_marshall_http_request(message, &marshaling_buf)) {
        aws_jni_throw_runtime_exception(
            env, "aws_java_http_request_from_native: %s.", aws_error_debug_str(aws_last_error()));
        goto done;
    }

    jni_request_blob = aws_jni_direct_byte_buffer_from_raw_ptr(env, marshaling_buf.buffer, marshaling_buf.len);

    /* Currently our only use case for this does not involve a body stream. We should come back and handle this
       when it's not time sensitive to do so. */
    j_request = (*env)->NewObject(
        env,
        http_request_properties.http_request_class,
        http_request_properties.constructor_method_id,
        jni_request_blob,
        request_body_stream);

    if (aws_jni_check_and_clear_exception(env)) {
        aws_raise_error(AWS_ERROR_HTTP_CALLBACK_FAILURE);
        goto done;
    }

done:
    if (jni_request_blob) {
        (*env)->DeleteLocalRef(env, jni_request_blob);
    }

    aws_byte_buf_clean_up(&marshaling_buf);
    return j_request;
}
