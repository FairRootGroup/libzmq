/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "precompiled.hpp"
#include "macros.hpp"
#include "stream.hpp"
#include "pipe.hpp"
#include "wire.hpp"
#include "random.hpp"
#include "likely.hpp"
#include "err.hpp"

zmq::stream_t::stream_t (class ctx_t *parent_, uint32_t tid_, int sid_) :
    routing_socket_base_t (parent_, tid_, sid_),
    _prefetched (false),
    _routing_id_sent (false),
    _current_out (NULL),
    _more_out (false),
    _next_integral_routing_id (generate_random ())
{
    options.type = ZMQ_STREAM;
    options.raw_socket = true;

    _prefetched_routing_id.init ();
    _prefetched_msg.init ();
}

zmq::stream_t::~stream_t ()
{
    zmq_assert (_out_pipes.empty ());
    _prefetched_routing_id.close ();
    _prefetched_msg.close ();
}

void zmq::stream_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_)
{
    LIBZMQ_UNUSED (subscribe_to_all_);

    zmq_assert (pipe_);

    identify_peer (pipe_);
    _fq.attach (pipe_);
}

void zmq::stream_t::xpipe_terminated (pipe_t *pipe_)
{
    out_pipes_t::iterator it = _out_pipes.find (pipe_->get_routing_id ());
    zmq_assert (it != _out_pipes.end ());
    _out_pipes.erase (it);
    _fq.pipe_terminated (pipe_);
    if (pipe_ == _current_out)
        _current_out = NULL;
}

void zmq::stream_t::xread_activated (pipe_t *pipe_)
{
    _fq.activated (pipe_);
}

void zmq::stream_t::xwrite_activated (pipe_t *pipe_)
{
    out_pipes_t::iterator it;
    for (it = _out_pipes.begin (); it != _out_pipes.end (); ++it)
        if (it->second.pipe == pipe_)
            break;

    zmq_assert (it != _out_pipes.end ());
    zmq_assert (!it->second.active);
    it->second.active = true;
}

int zmq::stream_t::xsend (msg_t *msg_)
{
    //  If this is the first part of the message it's the ID of the
    //  peer to send the message to.
    if (!_more_out) {
        zmq_assert (!_current_out);

        //  If we have malformed message (prefix with no subsequent message)
        //  then just silently ignore it.
        //  TODO: The connections should be killed instead.
        if (msg_->flags () & msg_t::more) {
            //  Find the pipe associated with the routing id stored in the prefix.
            //  If there's no such pipe return an error
            blob_t routing_id (static_cast<unsigned char *> (msg_->data ()),
                               msg_->size ());
            out_pipes_t::iterator it = _out_pipes.find (routing_id);

            if (it != _out_pipes.end ()) {
                _current_out = it->second.pipe;
                if (!_current_out->check_write ()) {
                    it->second.active = false;
                    _current_out = NULL;
                    errno = EAGAIN;
                    return -1;
                }
            } else {
                errno = EHOSTUNREACH;
                return -1;
            }
        }

        //  Expect one more message frame.
        _more_out = true;

        int rc = msg_->close ();
        errno_assert (rc == 0);
        rc = msg_->init ();
        errno_assert (rc == 0);
        return 0;
    }

    //  Ignore the MORE flag
    msg_->reset_flags (msg_t::more);

    //  This is the last part of the message.
    _more_out = false;

    //  Push the message into the pipe. If there's no out pipe, just drop it.
    if (_current_out) {
        // Close the remote connection if user has asked to do so
        // by sending zero length message.
        // Pending messages in the pipe will be dropped (on receiving term- ack)
        if (msg_->size () == 0) {
            _current_out->terminate (false);
            int rc = msg_->close ();
            errno_assert (rc == 0);
            rc = msg_->init ();
            errno_assert (rc == 0);
            _current_out = NULL;
            return 0;
        }
        bool ok = _current_out->write (msg_);
        if (likely (ok))
            _current_out->flush ();
        _current_out = NULL;
    } else {
        int rc = msg_->close ();
        errno_assert (rc == 0);
    }

    //  Detach the message from the data buffer.
    int rc = msg_->init ();
    errno_assert (rc == 0);

    return 0;
}

int zmq::stream_t::xsetsockopt (int option_,
                                const void *optval_,
                                size_t optvallen_)
{
    switch (option_) {
        case ZMQ_STREAM_NOTIFY:
            return do_setsockopt_int_as_bool_strict (optval_, optvallen_,
                                                     &options.raw_notify);

        default:
            return routing_socket_base_t::xsetsockopt (option_, optval_,
                                                       optvallen_);
    }
}

int zmq::stream_t::xrecv (msg_t *msg_)
{
    if (_prefetched) {
        if (!_routing_id_sent) {
            int rc = msg_->move (_prefetched_routing_id);
            errno_assert (rc == 0);
            _routing_id_sent = true;
        } else {
            int rc = msg_->move (_prefetched_msg);
            errno_assert (rc == 0);
            _prefetched = false;
        }
        return 0;
    }

    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (&_prefetched_msg, &pipe);
    if (rc != 0)
        return -1;

    zmq_assert (pipe != NULL);
    zmq_assert ((_prefetched_msg.flags () & msg_t::more) == 0);

    //  We have received a frame with TCP data.
    //  Rather than sending this frame, we keep it in prefetched
    //  buffer and send a frame with peer's ID.
    const blob_t &routing_id = pipe->get_routing_id ();
    rc = msg_->close ();
    errno_assert (rc == 0);
    rc = msg_->init_size (routing_id.size ());
    errno_assert (rc == 0);

    // forward metadata (if any)
    metadata_t *metadata = _prefetched_msg.metadata ();
    if (metadata)
        msg_->set_metadata (metadata);

    memcpy (msg_->data (), routing_id.data (), routing_id.size ());
    msg_->set_flags (msg_t::more);

    _prefetched = true;
    _routing_id_sent = true;

    return 0;
}

bool zmq::stream_t::xhas_in ()
{
    //  We may already have a message pre-fetched.
    if (_prefetched)
        return true;

    //  Try to read the next message.
    //  The message, if read, is kept in the pre-fetch buffer.
    pipe_t *pipe = NULL;
    int rc = _fq.recvpipe (&_prefetched_msg, &pipe);
    if (rc != 0)
        return false;

    zmq_assert (pipe != NULL);
    zmq_assert ((_prefetched_msg.flags () & msg_t::more) == 0);

    const blob_t &routing_id = pipe->get_routing_id ();
    rc = _prefetched_routing_id.init_size (routing_id.size ());
    errno_assert (rc == 0);

    // forward metadata (if any)
    metadata_t *metadata = _prefetched_msg.metadata ();
    if (metadata)
        _prefetched_routing_id.set_metadata (metadata);

    memcpy (_prefetched_routing_id.data (), routing_id.data (),
            routing_id.size ());
    _prefetched_routing_id.set_flags (msg_t::more);

    _prefetched = true;
    _routing_id_sent = false;

    return true;
}

bool zmq::stream_t::xhas_out ()
{
    //  In theory, STREAM socket is always ready for writing. Whether actual
    //  attempt to write succeeds depends on which pipe the message is going
    //  to be routed to.
    return true;
}

void zmq::stream_t::identify_peer (pipe_t *pipe_)
{
    //  Always assign routing id for raw-socket
    unsigned char buffer[5];
    buffer[0] = 0;
    blob_t routing_id;
    const std::string connect_routing_id = extract_connect_routing_id ();
    if (!connect_routing_id.empty ()) {
        routing_id.set (
          reinterpret_cast<const unsigned char *> (connect_routing_id.c_str ()),
          connect_routing_id.length ());
        //  Not allowed to duplicate an existing rid
        zmq_assert (0 == _out_pipes.count (routing_id));
    } else {
        put_uint32 (buffer + 1, _next_integral_routing_id++);
        routing_id.set (buffer, sizeof buffer);
        memcpy (options.routing_id, routing_id.data (), routing_id.size ());
        options.routing_id_size =
          static_cast<unsigned char> (routing_id.size ());
    }
    pipe_->set_router_socket_routing_id (routing_id);
    //  Add the record into output pipes lookup table
    out_pipe_t outpipe = {pipe_, true};
    const bool ok =
      _out_pipes.ZMQ_MAP_INSERT_OR_EMPLACE (ZMQ_MOVE (routing_id), outpipe)
        .second;
    zmq_assert (ok);
}
