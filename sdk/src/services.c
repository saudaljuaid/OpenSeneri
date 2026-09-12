/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/event.h>
#include <phipia/network.h>
#include <phipia/runtime.h>
#include <phipia/window.h>

#include <errno.h>
#include <string.h>

long phipia_wait(struct phipia_wait_item *items, size_t count,
    uint64_t deadline_ns)
{
    const struct phipia_wait_request request = {sizeof(request),
        PHIPIA_ABI_VERSION, (uint64_t)(uintptr_t)items, deadline_ns,
        (uint32_t)count, 0U};

    if (items == NULL || count == 0U || count > PHIPIA_WAIT_MAX) {
        return -PHIPIA_EINVAL;
    }
    return phipia_syscall1(PHIPIA_SYS_WAIT,
        (uint64_t)(uintptr_t)&request);
}

long phipia_timer_create(void)
{
    return phipia_syscall0(PHIPIA_SYS_TIMER_CREATE);
}

long phipia_timer_set(phipia_handle_t timer, uint64_t deadline_ns)
{
    const struct phipia_timer_set_request request = {sizeof(request),
        PHIPIA_ABI_VERSION, timer, deadline_ns, 0U, 0U};

    return phipia_syscall1(PHIPIA_SYS_TIMER_SET,
        (uint64_t)(uintptr_t)&request);
}

long phipia_cancel(phipia_handle_t handle)
{
    return phipia_syscall1(PHIPIA_SYS_CANCEL, handle);
}

int phipia_window_create(const char *title, uint32_t width, uint32_t height,
    struct phipia_window_create_response *response)
{
    if (title == NULL) {
        return phipia_result(-PHIPIA_EFAULT);
    }
    const struct phipia_window_create_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, (uint64_t)(uintptr_t)title,
        (uint32_t)strlen(title), width, height, PHIPIA_PIXEL_XRGB8888, 0U, 0U
    };
    return phipia_result(phipia_syscall2(PHIPIA_SYS_WINDOW_CREATE,
        (uint64_t)(uintptr_t)&request, (uint64_t)(uintptr_t)response));
}
long phipia_surface_present(phipia_handle_t window,
    const struct phipia_rect *rectangles, size_t count)
{
    const struct phipia_present_request request = {sizeof(request),
        PHIPIA_ABI_VERSION, window, (uint64_t)(uintptr_t)rectangles,
        (uint32_t)count, 0U};
    return phipia_syscall1(PHIPIA_SYS_SURFACE_PRESENT,
        (uint64_t)(uintptr_t)&request);
}
long phipia_event_read(phipia_handle_t events, struct phipia_event *event)
{ return phipia_syscall2(PHIPIA_SYS_EVENT_READ, events, (uint64_t)(uintptr_t)event); }
long phipia_event_wait(phipia_handle_t events, uint64_t deadline_ns)
{
    struct phipia_wait_item item = {events, PHIPIA_WAIT_READABLE, 0U};
    const struct phipia_wait_request request = {sizeof(request),
        PHIPIA_ABI_VERSION, (uint64_t)(uintptr_t)&item, deadline_ns, 1U, 0U};
    return phipia_syscall1(PHIPIA_SYS_WAIT, (uint64_t)(uintptr_t)&request);
}
long phipia_pointer_capture(phipia_handle_t window, int capture)
{ return phipia_syscall2(PHIPIA_SYS_POINTER_CAPTURE, window, capture != 0); }

long phipia_dns_resolve(const char *hostname, uint64_t deadline_ns)
{
    if (hostname == NULL) return -PHIPIA_EFAULT;
    return phipia_syscall3(PHIPIA_SYS_DNS_RESOLVE,
        (uint64_t)(uintptr_t)hostname, strlen(hostname), deadline_ns);
}
long phipia_stream_open(void) { return phipia_syscall0(PHIPIA_SYS_STREAM_OPEN); }
long phipia_stream_connect(phipia_handle_t stream,
    const struct phipia_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{ return phipia_syscall3(PHIPIA_SYS_STREAM_CONNECT, stream, (uint64_t)(uintptr_t)endpoint, deadline_ns); }
static long network_io(uint64_t number, phipia_handle_t handle, void *buffer,
    size_t length, uint64_t deadline, struct phipia_ipv4_endpoint *endpoint)
{
    struct phipia_network_io request;

    if (length == 0U || length > UINT32_MAX) {
        return -PHIPIA_EINVAL;
    }
    if ((number == PHIPIA_SYS_STREAM_READ ||
            number == PHIPIA_SYS_STREAM_WRITE) &&
        length > PHIPIA_NETWORK_IO_MAX_BYTES) {
        length = PHIPIA_NETWORK_IO_MAX_BYTES;
    }
    request = (struct phipia_network_io){sizeof(request), PHIPIA_ABI_VERSION,
        handle, (uint64_t)(uintptr_t)buffer, deadline, {0U, 0U, 0U},
        (uint32_t)length, 0U};
    if (endpoint != NULL) request.endpoint = *endpoint;
    const long result = phipia_syscall1(number, (uint64_t)(uintptr_t)&request);
    if (result >= 0 && endpoint != NULL &&
        number == PHIPIA_SYS_DATAGRAM_RECEIVE) *endpoint = request.endpoint;
    return result;
}
long phipia_stream_read(phipia_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{ return network_io(PHIPIA_SYS_STREAM_READ, stream, buffer, length, deadline_ns, NULL); }
long phipia_stream_write(phipia_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{ return network_io(PHIPIA_SYS_STREAM_WRITE, stream, (void *)(uintptr_t)buffer, length, deadline_ns, NULL); }
long phipia_stream_shutdown(phipia_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{ return phipia_syscall3(PHIPIA_SYS_STREAM_SHUTDOWN, stream, flags, deadline_ns); }
long phipia_datagram_open(void) { return phipia_syscall0(PHIPIA_SYS_DATAGRAM_OPEN); }
long phipia_datagram_bind(phipia_handle_t datagram, uint16_t port)
{ return phipia_syscall2(PHIPIA_SYS_DATAGRAM_BIND, datagram, port); }
long phipia_datagram_send(phipia_handle_t datagram,
    const struct phipia_ipv4_endpoint *destination, const void *buffer,
    size_t length, uint64_t deadline_ns)
{
    if (destination == NULL) return -PHIPIA_EFAULT;
    struct phipia_ipv4_endpoint endpoint = *destination;
    return network_io(PHIPIA_SYS_DATAGRAM_SEND, datagram,
        (void *)(uintptr_t)buffer, length, deadline_ns, &endpoint);
}
long phipia_datagram_receive(phipia_handle_t datagram,
    struct phipia_ipv4_endpoint *source, void *buffer, size_t length,
    uint64_t deadline_ns)
{ return network_io(PHIPIA_SYS_DATAGRAM_RECEIVE, datagram, buffer, length, deadline_ns, source); }
long phipia_network_address(phipia_handle_t handle, int peer,
    struct phipia_ipv4_endpoint *endpoint)
{ return phipia_syscall3(PHIPIA_SYS_NETWORK_ADDRESS, handle, peer != 0, (uint64_t)(uintptr_t)endpoint); }
long phipia_network_cancel(phipia_handle_t handle)
{ return phipia_syscall1(PHIPIA_SYS_CANCEL, handle); }
