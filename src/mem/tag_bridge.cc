/*
 * Copyright (c) 2011-2013 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Ali Saidi
 *          Steve Reinhardt
 *          Andreas Hansson
 */

/**
 * @file
 * Implementation of a memory-mapped bridge that connects a master
 * and a slave through a request and response queue.
 */

#include "base/trace.hh"
#include "debug/TagBridge.hh"
#include "params/TagBridge.hh"
#include "mem/tag_bridge.hh"

TagBridge::BridgeSlavePort::BridgeSlavePort(const std::string& _name,
                                         TagBridge& _bridge,
                                         BridgeMasterPort& _masterPort,
                                         Cycles _delay, int _resp_limit,
                                         std::vector<AddrRange> _ranges)
    : SlavePort(_name, &_bridge), bridge(_bridge), masterPort(_masterPort),
      delay(_delay), ranges(_ranges.begin(), _ranges.end()),
      outstandingResponses(0), retryReq(false),
      respQueueLimit(_resp_limit), sendEvent(*this)
{
}

TagBridge::BridgeMasterPort::BridgeMasterPort(const std::string& _name,
                                           TagBridge& _bridge,
                                           BridgeSlavePort& _slavePort,
                                           Cycles _delay, int _req_limit)
    : MasterPort(_name, &_bridge), bridge(_bridge), slavePort(_slavePort),
      delay(_delay), reqQueueLimit(_req_limit), sendEvent(*this)
{
}

TagBridge::TagBridge(Params *p)
    : MemObject(p),
      slavePort(p->name + ".slave", *this, masterPort,
                ticksToCycles(p->delay), p->resp_size, p->ranges),
      masterPort(p->name + ".master", *this, slavePort,
                 ticksToCycles(p->delay), p->req_size),
      DSid(p->DSid),
      DSid_base_addr(p->DSid_base_addr)
{
}

BaseMasterPort&
TagBridge::getMasterPort(const std::string &if_name, PortID idx)
{
    if (if_name == "master")
        return masterPort;
    else
        // pass it along to our super class
        return MemObject::getMasterPort(if_name, idx);
}

BaseSlavePort&
TagBridge::getSlavePort(const std::string &if_name, PortID idx)
{
    if (if_name == "slave")
        return slavePort;
    else
        // pass it along to our super class
        return MemObject::getSlavePort(if_name, idx);
}

void
TagBridge::init()
{
    // make sure both sides are connected and have the same block size
    if (!slavePort.isConnected() || !masterPort.isConnected())
        fatal("Both ports of a bridge must be connected.\n");

    // notify the master side  of our address ranges
    slavePort.sendRangeChange();
}

bool
TagBridge::BridgeSlavePort::respQueueFull() const
{
    return outstandingResponses == respQueueLimit;
}

bool
TagBridge::BridgeMasterPort::reqQueueFull() const
{
    return transmitList.size() == reqQueueLimit;
}

bool
TagBridge::BridgeMasterPort::recvTimingResp(PacketPtr pkt)
{
    // all checks are done when the request is accepted on the slave
    // side, so we are guaranteed to have space for the response
    DPRINTF(TagBridge, "recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    DPRINTF(TagBridge, "Request queue size: %d\n", transmitList.size());

    // @todo: We need to pay for this and not just zero it out
    pkt->firstWordDelay = pkt->lastWordDelay = 0;

    slavePort.schedTimingResp(pkt, bridge.clockEdge(delay));

    return true;
}

bool
TagBridge::BridgeSlavePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(TagBridge, "recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    // we should not see a timing request if we are already in a retry
    assert(!retryReq);

    DPRINTF(TagBridge, "Response queue size: %d outresp: %d\n",
            transmitList.size(), outstandingResponses);

    // if the request queue is full then there is no hope
    if (masterPort.reqQueueFull()) {
        DPRINTF(TagBridge, "Request queue full\n");
        retryReq = true;
    } else {
        // look at the response queue if we expect to see a response
        bool expects_response = pkt->needsResponse() &&
            !pkt->memInhibitAsserted();
        if (expects_response) {
            if (respQueueFull()) {
                DPRINTF(TagBridge, "Response queue full\n");
                retryReq = true;
            } else {
                // ok to send the request with space for the response
                DPRINTF(TagBridge, "Reserving space for response\n");
                assert(outstandingResponses != respQueueLimit);
                ++outstandingResponses;

                // no need to set retryReq to false as this is already the
                // case
            }
        }

        // attach DSid to pkt
        assert(!pkt->hasDSid());
        pkt->setDSid(bridge.DSid);

        if (!retryReq) {
            // @todo: We need to pay for this and not just zero it out
            pkt->firstWordDelay = pkt->lastWordDelay = 0;

            masterPort.schedTimingReq(pkt, bridge.clockEdge(delay));
        }
    }

    // remember that we are now stalling a packet and that we have to
    // tell the sending master to retry once space becomes available,
    // we make no distinction whether the stalling is due to the
    // request queue or response queue being full
    return !retryReq;
}

void
TagBridge::BridgeSlavePort::retryStalledReq()
{
    if (retryReq) {
        DPRINTF(TagBridge, "Request waiting for retry, now retrying\n");
        retryReq = false;
        sendRetry();
    }
}

void
TagBridge::BridgeMasterPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    // If we expect to see a response, we need to restore the source
    // and destination field that is potentially changed by a second
    // crossbar
    if (!pkt->memInhibitAsserted() && pkt->needsResponse()) {
        // Update the sender state so we can deal with the response
        // appropriately
        pkt->pushSenderState(new RequestState(pkt->getSrc()));
    }

    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        bridge.schedule(sendEvent, when);
    }

    assert(transmitList.size() != reqQueueLimit);

    transmitList.push_back(DeferredPacket(pkt, when));
}


void
TagBridge::BridgeSlavePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    // This is a response for a request we forwarded earlier.  The
    // corresponding request state should be stored in the packet's
    // senderState field.
    RequestState *req_state =
        dynamic_cast<RequestState*>(pkt->popSenderState());
    assert(req_state != NULL);
    pkt->setDest(req_state->origSrc);
    delete req_state;

    // the bridge sets the destination irrespective of it is valid or
    // not, as it is checked in the crossbar
    DPRINTF(TagBridge, "response, new dest %d\n", pkt->getDest());

    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        bridge.schedule(sendEvent, when);
    }

    transmitList.push_back(DeferredPacket(pkt, when));
}

void
TagBridge::BridgeMasterPort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket req = transmitList.front();

    assert(req.tick <= curTick());

    PacketPtr pkt = req.pkt;

    DPRINTF(TagBridge, "trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        // send successful
        transmitList.pop_front();
        DPRINTF(TagBridge, "trySend request successful\n");

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_req = transmitList.front();
            DPRINTF(TagBridge, "Scheduling next send\n");
            bridge.schedule(sendEvent, std::max(next_req.tick,
                                                bridge.clockEdge()));
        }

        // if we have stalled a request due to a full request queue,
        // then send a retry at this point, also note that if the
        // request we stalled was waiting for the response queue
        // rather than the request queue we might stall it again
        slavePort.retryStalledReq();
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
TagBridge::BridgeSlavePort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket resp = transmitList.front();

    assert(resp.tick <= curTick());

    PacketPtr pkt = resp.pkt;

    DPRINTF(TagBridge, "trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        // send successful
        transmitList.pop_front();
        DPRINTF(TagBridge, "trySend response successful\n");

        assert(outstandingResponses != 0);
        --outstandingResponses;

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_resp = transmitList.front();
            DPRINTF(TagBridge, "Scheduling next send\n");
            bridge.schedule(sendEvent, std::max(next_resp.tick,
                                                bridge.clockEdge()));
        }

        // if there is space in the request queue and we were stalling
        // a request, it will definitely be possible to accept it now
        // since there is guaranteed space in the response queue
        if (!masterPort.reqQueueFull() && retryReq) {
            DPRINTF(TagBridge, "Request waiting for retry, now retrying\n");
            retryReq = false;
            sendRetry();
        }
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
TagBridge::BridgeMasterPort::recvRetry()
{
    trySendTiming();
}

void
TagBridge::BridgeSlavePort::recvRetry()
{
    trySendTiming();
}

Tick
TagBridge::BridgeSlavePort::recvAtomic(PacketPtr pkt)
{
    // attach DSid to pkt
    assert(!pkt->hasDSid());
    pkt->setDSid(bridge.DSid);
    return delay * bridge.clockPeriod() + masterPort.sendAtomic(pkt);
}

void
TagBridge::BridgeSlavePort::recvFunctional(PacketPtr pkt)
{
    pkt->pushLabel(name());

    // attach DSid to pkt
    assert(!pkt->hasDSid());
    pkt->setDSid(bridge.DSid);

    // check the response queue
    for (auto i = transmitList.begin();  i != transmitList.end(); ++i) {
        if (pkt->checkFunctional((*i).pkt)) {
            pkt->makeResponse();
            return;
        }
    }

    // also check the master port's request queue
    if (masterPort.checkFunctional(pkt)) {
        return;
    }

    pkt->popLabel();

    // fall through if pkt still not satisfied
    masterPort.sendFunctional(pkt);
}

bool
TagBridge::BridgeMasterPort::checkFunctional(PacketPtr pkt)
{
    bool found = false;
    auto i = transmitList.begin();

    while(i != transmitList.end() && !found) {
        if (pkt->checkFunctional((*i).pkt)) {
            pkt->makeResponse();
            found = true;
        }
        ++i;
    }

    return found;
}

AddrRangeList
TagBridge::BridgeSlavePort::getAddrRanges() const
{
    return ranges;
}

TagBridge *
TagBridgeParams::create()
{
    return new TagBridge(this);
}