/*
 * Copyright (c) 2025, Edwin Hoksberg <mail@edwinhoksberg.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Serial/SerialPort.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Serial {

GC_DEFINE_ALLOCATOR(SerialPort);

SerialPort::SerialPort(JS::Realm& realm, serial_cpp::PortInfo device)
    : DOM::EventTarget(realm)
    , m_device(move(device))
{
}

void SerialPort::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SerialPort);
    Base::initialize(realm);
}

// https://wicg.github.io/serial/#getinfo-method
SerialPortInfo SerialPort::get_info() const
{
    // 1. Let info be an empty ordered map.
    auto info = SerialPortInfo {};

    // FIXME: 2. If the port is part of a USB device, perform the following steps:
    {
        // FIXME: 1. Set info["usbVendorId"] to the vendor ID of the device.

        // FIXME: 2. Set info["usbProductId"] to the product ID of the device.
    }

    // FIXME: 3. If the port is a service on a Bluetooth device, perform the following steps:
    {
        // FIXME: 1. Set info["bluetoothServiceClassId"] to the service class UUID of the Bluetooth service.
    }

    // 4. Return info.
    return info;
}

// https://wicg.github.io/serial/#open-method
GC::Ref<WebIDL::Promise> SerialPort::open(SerialOptions const& options)
{
    dbgln_if(LIBWEB_SERIAL_DEBUG, "SerialPort::open() called on port {} with: baud_rate={}, data_bits={}, stop_bits={}, parity={}, buffer_size={}, flow_control={}",
        m_device.port.c_str(), options.baud_rate, options.data_bits, options.stop_bits, Bindings::idl_enum_to_string(options.parity.value()),
        options.buffer_size, Bindings::idl_enum_to_string(options.flow_control.value()));

    auto& realm = this->realm();

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. If this.[[state]] is not "closed", reject promise with an "InvalidStateError" DOMException and return promise.
    if (m_state != SerialPortState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Failed to execute 'open' on 'SerialPort': The port is already open."_utf16));
        return promise;
    }

    // 3. If options["dataBits"] is not 7 or 8, reject promise with TypeError and return promise.
    if (options.data_bits != 7 && options.data_bits != 8) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'open' on 'SerialPort': Requested number of data bits must be 7 or 8."_utf16));
        return promise;
    }

    // 4. If options["stopBits"] is not 1 or 2, reject promise with TypeError and return promise.
    if (options.stop_bits != 1 && options.stop_bits != 2) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'open' on 'SerialPort': Requested number of stop bits must be 1 or 2."_utf16));
        return promise;
    }

    // 5. If options["bufferSize"] is 0, reject promise with TypeError and return promise.
    if (options.buffer_size == static_cast<u64>(0)) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'open' on 'SerialPort': Buffer size must be greater than 0."_utf16));
        return promise;
    }

    // 6. Optionally, if options["bufferSize"] is larger than the implementation is able to support, reject promise with a TypeError and return promise.
    if (options.buffer_size.value() > NumericLimits<u32>::max()) {
        WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "Failed to execute 'open' on 'SerialPort': Buffer size exceeds maximum supported size."_utf16));
        return promise;
    }

    // 7. Set this.[[state]] to "opening".
    m_state = SerialPortState::Opening;

    // 8. Perform the following steps in parallel.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise, options]() -> void {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        serial_cpp::parity_t parity_type;
        switch (options.parity.value()) {
        case Bindings::ParityType::Even:
            parity_type = serial_cpp::parity_even;
            break;
        case Bindings::ParityType::Odd:
            parity_type = serial_cpp::parity_odd;
            break;
        case Bindings::ParityType::None:
            parity_type = serial_cpp::parity_none;
            break;
        }
        auto data_bits = options.data_bits == 8 ? serial_cpp::eightbits : serial_cpp::sevenbits;
        auto stop_bits = options.stop_bits == 1 ? serial_cpp::stopbits_one : serial_cpp::stopbits_two;
        auto flow_control = options.flow_control.value() == Bindings::FlowControlType::None ? serial_cpp::flowcontrol_none : serial_cpp::flowcontrol_hardware;

        // 1. Invoke the operating system to open the serial port using the connection parameters (or their defaults) specified in options.
        m_port = std::make_unique<serial_cpp::Serial>(
            m_device.port,
            options.baud_rate,
            serial_cpp::Timeout::simpleTimeout(1000),
            data_bits,
            parity_type,
            stop_bits,
            flow_control);

        // 2. If this fails for any reason, queue a global task on the relevant global object of this using the serial port task source to reject promise with a "NetworkError" DOMException and abort these steps.
        if (!m_port) {
            queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() {
                HTML::TemporaryExecutionContext context(realm);
                WebIDL::reject_promise(realm, promise, WebIDL::NetworkError::create(realm, "Failed to execute 'requestPort' on 'Serial': Failed to open port."_utf16));
            }));
            return;
        }

        // 3. Set this.[[state]] to "opened".
        m_state = SerialPortState::Opened;

        // 4. Set this.[[bufferSize]] to options["bufferSize"].
        m_buffer_size = options.buffer_size.value();

        // 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
        queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() {
            HTML::TemporaryExecutionContext context(realm);
            WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        }));
    }));

    // 9. Return promise.
    return promise;
}

// https://wicg.github.io/serial/#setsignals-method
GC::Ref<WebIDL::Promise> SerialPort::set_signals(SerialOutputSignals)
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. If all of the specified members of signals are not present reject promise with TypeError and return promise.

    // FIXME: 4. Perform the following steps in parallel:
    {
        // FIXME: 1. If signals["dataTerminalReady"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "data terminal ready" or "DTR" signal on the serial port.

        // FIXME: 2. If signals["requestToSend"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "request to send" or "RTS" signal on the serial port.

        // FIXME: 3. If signals["break"] is present, invoke the operating system to either assert (if true) or
        //           deassert (if false) the "break" signal on the serial port.

        // FIXME: 4. If the operating system fails to change the state of any of these signals for any reason, queue a global task
        //           on the relevant global object of this using the serial port task source to reject promise with a "NetworkError" DOMException.

        // FIXME: 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
    }

    // 5. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::set_signals()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#getsignals-method
GC::Ref<WebIDL::Promise> SerialPort::get_signals() const
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.

    // FIXME: 3. Perform the following steps in parallel:
    {
        // FIXME: 1. Query the operating system for the status of the control signals that may be asserted by the device connected to the serial port.

        // FIXME: 2. If the operating system fails to determine the status of these signals for any reason, queue a global task on the relevant global object of
        //           this using the serial port task source to reject promise with a "NetworkError" DOMException and abort these steps.

        // FIXME: 3. Let dataCarrierDetect be true if the "data carrier detect" or "DCD" signal has been asserted by the device, and false otherwise.

        // FIXME: 4. Let clearToSend be true if the "clear to send" or "CTS" signal has been asserted by the device, and false otherwise.

        // FIXME: 5. Let ringIndicator be true if the "ring indicator" or "RI" signal has been asserted by the device, and false otherwise.

        // FIXME: 6. Let dataSetReady be true if the "data set ready" or "DSR" signal has been asserted by the device, and false otherwise.

        // FIXME: 7. Let signals be the ordered map «[ "dataCarrierDetect" → dataCarrierDetect, "clearToSend" → clearToSend, "ringIndicator" → ringIndicator, "dataSetReady" → dataSetReady ]».

        // FIXME: 8. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with signals.
    }

    // 4. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::get_signals()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#close-method
GC::Ref<WebIDL::Promise> SerialPort::close()
{
    dbgln_if(LIBWEB_SERIAL_DEBUG, "SerialPort::close() called, closing port: {}", m_device.port.c_str());

    auto& realm = this->realm();

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. If this.[[state]] is not "opened", reject promise with an "InvalidStateError" DOMException and return promise.
    if (m_state != SerialPortState::Opened) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Failed to execute 'close' on 'SerialPort': The port is not open."_utf16));
        return promise;
    }

    // 3. Let cancelPromise be the result of invoking cancel on this.[[readable]] or a promise resolved with undefined if this.[[readable]] is null.
    auto cancel_promise = m_readable
        ? m_readable->cancel(JS::js_undefined())
        : WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 4. Let abortPromise be the result of invoking abort on this.[[writable]] or a promise resolved with undefined if this.[[writable]] is null.
    auto abort_promise = m_writable
        ? m_writable->abort(JS::js_undefined())
        : WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 5. Let pendingClosePromise be a new promise.
    auto pending_close_promise = WebIDL::create_promise(realm);

    // 6. If this.[[readable]] and this.[[writable]] are null, resolve pendingClosePromise with undefined.
    if (!m_readable && !m_writable) {
        WebIDL::resolve_promise(realm, pending_close_promise, JS::js_undefined());
    }

    // 7. Set this.[[pendingClosePromise]] to pendingClosePromise.
    m_pending_close_promise = pending_close_promise;

    // 8. Let combinedPromise be the result of getting a promise to wait for all with «cancelPromise, abortPromise, pendingClosePromise».
    auto combined_promise = WebIDL::get_promise_for_wait_for_all(realm, { cancel_promise, abort_promise, pending_close_promise });

    // 9. Set this.[[state]] to "closing".
    m_state = SerialPortState::Closing;

    // 10. React to combinedPromise.
    WebIDL::react_to_promise(combined_promise,
        // If combinedPromise was fulfilled, then:
        GC::create_function(realm.heap(), [this, &realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Run the following steps in parallel:
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [this, &realm, promise]() -> void {
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

                // 1. Invoke the operating system to close the serial port and release any associated resources.
                m_port->close();
                m_port.reset();

                // 2. Set this.[[state]] to "closed".
                m_state = SerialPortState::Closed;

                // 3. Set this.[[readFatal]] and this.[[writeFatal]] to false.
                m_read_fatal = false;
                m_write_fatal = false;

                // 4. Set this.[[pendingClosePromise]] to null.
                m_pending_close_promise = nullptr;

                // 5. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
                queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() {
                    HTML::TemporaryExecutionContext context(realm);
                    WebIDL::resolve_promise(realm, promise, JS::js_undefined());
                }));
            }));

            return value;
        }),

        // If combinedPromise was rejected with reason r, then:
        GC::create_function(realm.heap(), [this, &realm, promise](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            // 1. Set this.[[pendingClosePromise]] to null.
            m_pending_close_promise = nullptr;

            // 2. Queue a global task on the relevant global object of this using the serial port task source to reject promise with r.
            queue_global_task(HTML::Task::Source::SerialPort, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, value]() {
                HTML::TemporaryExecutionContext context(realm);
                WebIDL::reject_promise(realm, promise, value);
            }));

            return value;
        }));

    // 11. Return promise.
    return promise;
}

// https://wicg.github.io/serial/#forget-method
GC::Ref<WebIDL::Promise> SerialPort::forget()
{
    auto& realm = this->realm();

    // FIXME: 1. Let promise be a new promise.

    // FIXME: 1. If the user agent can't perform this action (e.g. permission was granted by administrator policy), return a promise resolved with undefined.

    // FIXME: 2. Run the following steps in parallel:
    {
        // FIXME: 1. Set this.[[state]] to "forgetting".

        // FIXME: 2. Remove this from the sequence of serial ports on the system which the user has allowed the site to access as the result of a previous call to requestPort().

        // FIXME: 3. Set this.[[state]] to "forgotten".

        // FIXME: 4. Queue a global task on the relevant global object of this using the serial port task source to resolve promise with undefined.
    }

    // 7. Return promise.
    dbgln("FIXME: Unimplemented SerialPort::forget()");
    return WebIDL::create_rejected_promise(realm, WebIDL::UnknownError::create(realm, Utf16String {}));
}

// https://wicg.github.io/serial/#readable-attribute
GC::Ptr<Streams::ReadableStream> SerialPort::readable()
{
    // 1. If this.[[readable]] is not null, return this.[[readable]].
    if (m_readable)
        return *m_readable;

    // 2. If this.[[state]] is not "opened", return null.
    if (m_state != SerialPortState::Opened)
        return nullptr;

    // 3. If this.[[readFatal]] is true, return null.
    if (m_read_fatal)
        return nullptr;

    auto& realm = this->realm();

    // 4. Let stream be a new ReadableStream.
    auto stream = realm.create<Streams::ReadableStream>(realm);

    // 5. Let pullAlgorithm be the following steps:
    auto pull_algorithm = GC::create_function(realm.heap(), [this, &realm]() {
        // 1. Let desiredSize be the desired size to fill up to the high water mark for this.[[readable]].
        auto desired_size = 65535;

        // 2. If this.[[readable]]'s current BYOB request view is non-null, then set desiredSize to this.[[readable]]'s current BYOB request view's byte length.
        if (auto byob_view = m_readable->current_byob_request_view())
            desired_size = byob_view->byte_length();

        // 3. Run the following steps in parallel:
        {
            // 1. Invoke the operating system to read up to desiredSize bytes from the port, putting the result in the byte sequence bytes.

            // 2. Queue a global task on the relevant global object of this using the serial port task source to run the following steps:
            {
                // 1. If no errors were encountered, then:
                {
                    // 1. If this.[[readable]]'s current BYOB request view is non-null, then write bytes into this.[[readable]]'s current BYOB request view, and set view to this.[[readable]]'s current BYOB request view.

                    // 2. Otherwise, set view to the result of creating a Uint8Array from bytes in this's relevant Realm.

                    // 3. Enqueue view into this.[[readable]].
                }
            }

            // 2. If a buffer overrun condition was encountered, invoke error on this.[[readable]] with a "BufferOverrunError" DOMException and invoke the steps to handle closing the readable stream.

            // 3. If a break condition was encountered, invoke error on this.[[readable]] with a "BreakError" DOMException and invoke the steps to handle closing the readable stream.

            // 4. If a framing error was encountered, invoke error on this.[[readable]] with a "FramingError" DOMException and invoke the steps to handle closing the readable stream.

            // 5. If a parity error was encountered, invoke error on this.[[readable]] with a "ParityError" DOMException and invoke the steps to handle closing the readable stream.

            // 6. If an operating system error was encountered, invoke error on this.[[readable]] with an "UnknownError" DOMException and invoke the steps to handle closing the readable stream.

            // 7. If the port was disconnected, run the following steps:
            {
                // 1. Set this.[[readFatal]] to true,

                // 2. Invoke error on this.[[readable]] with a "NetworkError" DOMException.

                // 3. Invoke the steps to handle closing the readable stream.
            }
        }

        // 4. Return a promise resolved with undefined.
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());
    });

    // 6. Let cancelAlgorithm be the following steps:
    auto cancel_algorithm = GC::create_function(realm.heap(), [&realm](JS::Value reason) {
        (void)reason;
        // 1. Let promise be a new promise.
        auto promise = WebIDL::create_promise(realm);

        // 2. Run the following steps in parallel.
        {
            // 1. Invoke the operating system to discard the contents of all software and hardware receive buffers for the port.

            // 2. Queue a global task on the relevant global object of this using the serial port task source to run the following steps:
            {
                // 1. Invoke the steps to handle closing the readable stream.

                // 2. Resolve promise with undefined.
            }
        }

        // 3. Return promise.
        return promise;
    });

    // 7. Set up with byte reading support stream with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm, and highWaterMark set to this.[[bufferSize]].
    m_readable->set_up_with_byte_reading_support(pull_algorithm, cancel_algorithm, m_buffer_size);

    // 8. Set this.[[readable]] to stream.
    m_readable = stream;

    // 9. Return stream.
    return stream;

    // FIXME: To handle closing the readable stream perform the following steps:
    {
        // 1. Set this.[[readable]] to null.

        // 2. If this.[[writable]] is null and this.[[pendingClosePromise]] is not null, resolve this.[[pendingClosePromise]] with undefined.
    }
}

// https://wicg.github.io/serial/#writable-attribute
GC::Ptr<Streams::WritableStream> SerialPort::writable()
{
    return *m_writable;

    // 1. If this.[[writable]] is not null, return this.[[writable]].

    // 2. If this.[[state]] is not "opened", return null.

    // 3. If this.[[writeFatal]] is true, return null.

    // 4. Let stream be a new WritableStream.

    // 5. Let signal be stream's signal.

    // 6. Let writeAlgorithm be the following steps, given chunk:
    {
        // 1. Let promise be a new promise.

        // 2. Assert: signal is not aborted.

        // 3. If chunk cannot be converted to an IDL value of type BufferSource, reject promise with a TypeError and return promise. Otherwise, save the result of the conversion in source.

        // 4. Get a copy of the buffer source source and save the result in bytes.

        // 5. In parallel, run the following steps:
        {
            // 1. Invoke the operating system to write bytes to the port. Alternately, store the chunk for future coalescing.

            // 2. Queue a global task on the relevant global object of this using the serial port task source to run the following steps:
            {
                // 1. If the chunk was successfully written, or was stored for future coalescing, resolve promise with undefined.

                // 2. If an operating system error was encountered, reject promise with an "UnknownError" DOMException.

                // 3. If the port was disconnected, run the following steps:
                {
                    // 1. Set this.[[writeFatal]] to true.

                    // 2. Reject promise with a "NetworkError" DOMException.

                    // 3. Invoke the steps to handle closing the writable stream.
                }

                // 4. If signal is aborted, reject promise with signal's abort reason.
            }
        }

        // 6. Return promise.
    }

    // 7. Let abortAlgorithm be the following steps:
    {
        // 1. Let promise be a new promise.

        // 2. Run the following steps in parallel.
        {
            // 1. Invoke the operating system to discard the contents of all software and hardware transmit buffers for the port.

            // 2. Queue a global task on the relevant global object of this using the serial port task source to run the following steps:
            {
                // 1. Invoke the steps to handle closing the writable stream.

                // 2. Resolve promise with undefined.
            }
        }

        // 3. Return promise.
    }

    // 8. Let closeAlgorithm be the following steps:
    {
        // 1. Let promise be a new promise.

        // 2. Run the following steps in parallel.
        {
            // 1. Invoke the operating system to flush the contents of all software and hardware transmit buffers for the port.

            // 2. Queue a global task on the relevant global object of this using the serial port task source to run the following steps:
            {
                // 1. Invoke the steps to handle closing the writable stream.

                // 2. If signal is aborted, reject promise with signal's abort reason.

                // 3. Otherwise, resolve promise with undefined.
            }
        }

        // 3. Return promise.
    }

    // 9. Set up stream with writeAlgorithm set to writeAlgorithm, abortAlgorithm set to abortAlgorithm, closeAlgorithm set to closeAlgorithm, highWaterMark set to this.[[bufferSize]], and sizeAlgorithm set to a byte-counting size algorithm.

    // 10. Add the following abort steps to signal:
    {
        // 1. Cause any invocation of the operating system to write to the port to return as soon as possible no matter how much data has been written.
    }

    // 11. Set this.[[writable]] to stream.

    // 12. Return stream.
}

void SerialPort::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_readable);
    visitor.visit(m_writable);
    visitor.visit(m_pending_close_promise);
}

// https://wicg.github.io/serial/#onconnect-attribute-0
void SerialPort::set_onconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::connect, event_handler);
}

WebIDL::CallbackType* SerialPort::onconnect()
{
    return event_handler_attribute(HTML::EventNames::connect);
}

// https://wicg.github.io/serial/#ondisconnect-attribute-0
void SerialPort::set_ondisconnect(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::disconnect, event_handler);
}

WebIDL::CallbackType* SerialPort::ondisconnect()
{
    return event_handler_attribute(HTML::EventNames::disconnect);
}

}
