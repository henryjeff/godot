/**************************************************************************/
/*  enet_godot_ext.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

/**
 @ *file  enet_godot_ext.h
 @brief ENet Godot extensions header
 */

#ifndef __ENET_GODOT_EXT_H__
#define __ENET_GODOT_EXT_H__

/** Sets the host field in the address parameter from ip struct.
    @param address destination to store resolved address
    @param ip the ip struct to read from
    @param size the size of the ip struct.
    @retval 0 on success
    @retval != 0 on failure
    @returns the address of the given ip in address on success.
*/
ENET_API void enet_address_set_ip(ENetAddress * address, const uint8_t * ip, size_t size);

ENET_API int enet_host_dtls_server_setup (ENetHost *, void *);
ENET_API int enet_host_dtls_client_setup (ENetHost *, const char *, void *);
ENET_API void enet_host_refuse_new_connections (ENetHost *, int);
ENET_API void enet_peer_socket_bind (ENetPeer *);
ENET_API void enet_peer_socket_destroy (ENetPeer *);

/* Fridge dev-only network-condition simulator (latency / jitter / loss).
   Injected at the Godot socket layer (enet_godot.cpp) so ENet's own RTT
   estimation, retransmission and throttle react to the simulated link the
   same way they would on a real WAN. Keyed by peer address (host[16]+port):
   on a listen server every client funnels through the one host socket, so
   per-client config + symmetric (send+recv) delay yields a faithful RTT with
   all simulation living on the host. rtt_ms is full round-trip; the simulator
   applies rtt_ms/2 on each crossing. Passing rtt/jitter/loss all <= 0 clears
   the entry; with no entries the socket path is byte-for-byte the original. */
ENET_API void enet_godot_net_sim_set (const uint8_t * host, uint16_t port, float rtt_ms, float jitter_ms, float loss);
ENET_API void enet_godot_net_sim_clear_all (void);

#endif // __ENET_GODOT_EXT_H__
