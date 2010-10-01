/* Copyright 2010 Jukka Jyl�nki

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

/** @file UDPMessageConnection.cpp
	@brief Implements the UDP-specific code of MessageConnection.
	\todo Flow control currently disabled since testing out the performance of UDT. */

#include <cmath>

#include "kNet/UDPMessageConnection.h"
#include "kNet/NetworkLogging.h"
#include "kNet/DataSerializer.h"
#include "kNet/DataDeserializer.h"
#include "kNet/VLEPacker.h"

#include "kNet/Sort.h"

using namespace std;

namespace kNet
{

static const int initialDatagramRatePerSecond = 30;
/// The maximum time to wait before acking a packet. If there are enough packets to ack for a full ack message,
/// acking will be performed earlier. (milliseconds)
static const float maxAckDelay = 33.f; // (1/30th of a second)
/// The time counter after which an unacked reliable message will be resent. (UDP only)
static const float timeOutMilliseconds = 2000.f;//750.f;
/// The maximum number of datagrams to read in from the socket at one go - after this reads will be throttled
/// to give time for data sending as well.
static const int cMaxDatagramsToReadInOneFrame = 2048;

static const u32 cMaxUDPMessageFragmentSize = 470;

UDPMessageConnection::UDPMessageConnection(Network *owner, NetworkServer *ownerServer, Socket *socket, ConnectionState startingState)
:MessageConnection(owner, ownerServer, socket, startingState),
retransmissionTimeout(3.f), smoothedRTT(3.f), rttVariation(0.f), rttCleared(true), // Set RTT initial values as per RFC 2988.
lastReceivedInOrderPacketID(0), 
lastSentInOrderPacketID(0), datagramPacketIDCounter(0),
packetLossRate(0.f), packetLossCount(0.f), datagramOutRatePerSecond(initialDatagramRatePerSecond), 
datagramInRatePerSecond(initialDatagramRatePerSecond),
datagramSendRate(10),
receivedPacketIDs(64 * 1024), outboundPacketAckTrack(1024)
{
}

UDPMessageConnection::~UDPMessageConnection()
{
	while(outboundPacketAckTrack.Size() > 0)
		FreeOutboundPacketAckTrack(outboundPacketAckTrack.Front()->packetID);

	outboundPacketAckTrack.Clear();
}

UDPMessageConnection::SocketReadResult UDPMessageConnection::ReadSocket(size_t &bytesRead)
{
	assert(socket->TransportLayer() == SocketOverUDP);

	if (isSlaveSocket)
		return SocketReadOK;

	SocketReadResult readResult = SocketReadOK;
		
	readResult = UDPReadSocket(bytesRead);

	///\todo Replace with ConnectSyn,ConnectSynAck and ConnectAck.
	if (bytesRead > 0 && connectionState == ConnectionPending)
	{
		connectionState = ConnectionOK;
		LOG(LogUser, "Established connection to socket %s.", socket->ToString().c_str());
	}
	if (readResult == SocketReadError)
		return SocketReadError;
	if (readResult == SocketReadThrottled)
		return SocketReadThrottled;
	if (bytesRead > 0)
		LOG(LogData, "Received %d bytes from UDP socket.", bytesRead);
	return SocketReadOK;
}

void UDPMessageConnection::Initialize()
{
	// Set RTT initial values as per RFC 2988.
	rttCleared = true;
	retransmissionTimeout = 3.f;
	smoothedRTT = 3.f;
	rttVariation = 0.f;

	datagramSendRate = 70.f; // At start, send one datagram per second.
	lastFrameTime = Clock::Tick();

	lastDatagramSendTime = Clock::Tick();
}

void UDPMessageConnection::PerformPacketAckSends()
{
	tick_t now = Clock::Tick();
	while(inboundPacketAckTrack.size() > 0)
	{
		if (Clock::TimespanToMillisecondsF(inboundPacketAckTrack.begin()->second.sentTick, now) < maxAckDelay &&
			inboundPacketAckTrack.size() < 33)
			break;

		SendPacketAckMessage();
	}
}

UDPMessageConnection::SocketReadResult UDPMessageConnection::UDPReadSocket(size_t &totalBytesRead)
{
	if (!socket || !socket->IsReadOpen())
		return SocketReadError;

	totalBytesRead = 0;

	// Read in all the bytes that are available in the socket.

	// Cap the number of datagrams to read in a single loop to perform throttling.
	int maxReads = cMaxDatagramsToReadInOneFrame;
	while(maxReads-- > 0)
	{
		OverlappedTransferBuffer *data = socket->BeginReceive();
		if (!data || data->bytesContains == 0)
			break;

		totalBytesRead += data->bytesContains;

		LOG(LogData, "UDPReadSocket: Received %d bytes from Begin/EndReceive.", data->bytesContains);
		ExtractMessages(data->buffer.buf, data->bytesContains);

		// Done with the received data buffer. Free it up for a future socket read.
		socket->EndReceive(data);
	}

	if (totalBytesRead > 0)
		AddInboundStats(totalBytesRead, 0, 0);

	if (maxReads == 0)
	{
		LOGNET("Warning: Too many inbound messages: Datagram read loop throttled!");
		return SocketReadThrottled;
	}
	else
		return SocketReadOK;
}

void UDPMessageConnection::SetUDPSlaveMode(bool enabled)
{
	isSlaveSocket = enabled;
}

/// Checks whether any reliably sent packets have timed out.
void UDPMessageConnection::ProcessPacketTimeouts() // [worker thread]
{
	assert(socket->TransportLayer() == SocketOverUDP);

	const tick_t now = Clock::Tick();

	int numPacketsTimedOut = 0;

	// Check whether any reliable packets have timed out and not acked.
	while(outboundPacketAckTrack.Size() > 0)
	{
		PacketAckTrack *track = outboundPacketAckTrack.Front();
		if (!track || Clock::IsNewer(track->timeoutTick, now))
			return; // Note here: for optimization purposes, the packets will time out in the order they were sent.

		++numPacketsTimedOut;
			
		LOG(LogVerbose, "A packet with ID %d timed out. Age: %.2fms. Contains %d messages.", 
			track->packetID, (float)Clock::TimespanToMillisecondsD(track->sentTick, now), track->messages.size());

		// Store a new suggestion for a lowered datagram send rate.
		lowestDatagramSendRateOnPacketLoss = min(lowestDatagramSendRateOnPacketLoss, track->datagramSendRate);

		// Adjust the flow control values on this event.
		UpdateRTOCounterOnPacketLoss();

		// Put all messages back into the outbound queue for send repriorisation.
		for(size_t i = 0; i < track->messages.size(); ++i)
			outboundQueue.InsertWithResize(track->messages[i]);

		// We are not going to resend the old timed out packet as-is with the old packet ID. Instead, just forget about it.
		// The messages will go to a brand new packet with new packet ID.
		outboundPacketAckTrack.PopFront();
	}
}

void UDPMessageConnection::HandleFlowControl()
{
	// In packets/second.
	const float totalEstimatedBandwidth = 50; ///\todo Make this estimation dynamic as in UDT or similar.
	const float additiveIncreaseAggressiveness = 5e-2f;

	const tick_t frameLength = Clock::TicksPerSec() / 100; // in ticks
	// Additively increase the outbound send rate.
	unsigned long numFrames = (unsigned long)(Clock::TicksInBetween(Clock::Tick(), lastFrameTime) / frameLength);
	if (/*numAcksLastFrame > 0 &&*/ numFrames > 0)
	{
		if (numFrames >= 100)
			numFrames = 100;

		if (numLossesLastFrame > 5) // Do not respond to a random single packet losses.
		{
			float oldRate = datagramSendRate;
			datagramSendRate = min(datagramSendRate, max(1.f, lowestDatagramSendRateOnPacketLoss * 0.9f)); // Multiplicative decreases.
//			datagramSendRate = max(1.f, datagramSendRate * 0.9f); // Multiplicative decreases.
			LOG(LogVerbose, "Received %d losses. datagramSendRate backed to %.2f from %.2f", numLossesLastFrame, datagramSendRate, oldRate);
		}
		else // Additive increases.
		{
			float increment = min((float)numFrames * additiveIncreaseAggressiveness * (totalEstimatedBandwidth - datagramSendRate), 1.f);
			datagramSendRate += increment;
			datagramSendRate = min(datagramSendRate, totalEstimatedBandwidth);
			lowestDatagramSendRateOnPacketLoss = datagramSendRate;
			LOG(LogVerbose, "Incremented sendRate by %.2f to %.2f", increment, datagramSendRate);
		}
		numAcksLastFrame = 0;
		numLossesLastFrame = 0;
		if (numFrames < 100)
			lastFrameTime += numFrames * frameLength;
		else
			lastFrameTime = Clock::Tick();
	}
}

void UDPMessageConnection::SendOutPackets()
{
	PacketSendResult result = PacketSendOK;
	int maxSends = 50;
	while(result == PacketSendOK && TimeUntilCanSendPacket() == 0 && maxSends-- > 0)
		result = SendOutPacket();
}

/// Packs several messages from the outbound priority queue into a single packet and sends it out the wire.
/// @return False if the send was a failure and sending should not be tried again at this time, true otherwise.
MessageConnection::PacketSendResult UDPMessageConnection::SendOutPacket()
{
	if (!socket || !socket->IsWriteOpen())
		return PacketSendSocketClosed;

	// If the main thread has asked the worker thread to hold sending any messages, stop here already.
	if (bOutboundSendsPaused)
		return PacketSendNoMessages;

	if (outboundQueue.Size() == 0)
		return PacketSendNoMessages;

	// If we aren't yet allowed to send out the next datagram, return.
	if (!CanSendOutNewDatagram())
		return PacketSendThrottled;

	OverlappedTransferBuffer *data = socket->BeginSend();
	if (!data)
		return PacketSendThrottled;

	const size_t minSendSize = 1;
	const size_t maxSendSize = socket->MaxSendSize();

	// Push out all the pending data to the socket.
	datagramSerializedMessages.clear();

	// If true, the receiver needs to Ack the packet we are now crafting.
	bool reliable = false;
	// If true, the packet contains in-order deliverable messages.
	bool inOrder = false;

	int packetSizeInBytes = 3; // PacketID + Flags take at least three bytes to start with.
	const int cBytesForInOrderDeltaCounter = 2;

	unsigned long smallestReliableMessageNumber = 0xFFFFFFFF;

	skippedMessages.clear();

	// Fill up the rest of the packet from messages from the outbound queue.
	while(outboundQueue.Size() > 0)
	{
		NetworkMessage *msg = *outboundQueue.Front();
		if (msg->obsolete)
		{
			outboundQueue.PopFront();
			FreeMessage(msg);
			continue;
		}

		// If we're sending a fragmented message, allocate a new transferID for that message,
		// or skip it if there are no transferIDs free.
		if (msg->transfer)
		{
			LOGNET("Sending out a fragmented transfer.");
			Lock<FragmentedSendManager> sends = fragmentedSends.Acquire();
			if (msg->transfer->id == -1)
			{
				bool success = sends->AllocateFragmentedTransferID(*msg->transfer);

				if (!success) // No transferIDs free - skip this message for now.
				{
					LOGNET("Throttling fragmented transfer send! No free TransferID to start a new fragmented transfer with!");
					outboundQueue.PopFront();
					skippedMessages.push_back(msg);
					continue;
				}
			}
		}

		// We need to add extra 2 bytes for the VLE-encoded InOrder PacketID delta counter.
		int totalMessageSize = msg->GetTotalDatagramPackedSize() + ((msg->inOrder && !inOrder) ? cBytesForInOrderDeltaCounter : 0);

		// If this message won't fit into the buffer, send out all the previously gathered messages.
		if ((size_t)packetSizeInBytes >= minSendSize && (size_t)packetSizeInBytes + totalMessageSize >= maxSendSize)
			break;

		datagramSerializedMessages.push_back(msg);
		outboundQueue.PopFront();

		packetSizeInBytes += totalMessageSize;

		if (msg->reliable)
		{
			reliable = true;
			smallestReliableMessageNumber = min(smallestReliableMessageNumber, msg->reliableMessageNumber);
		}

		if (msg->inOrder)
			inOrder = true;
	}

	// If we had skipped any messages from the outbound queue while looking for good messages to send, put all the messages
	// we skipped back to the outbound queue to wait to be processed during subsequent frames.
	for(size_t i = 0; i < skippedMessages.size(); ++i)
		outboundQueue.InsertWithResize(skippedMessages[i]);

	// Finally proceed to crafting the actual UDP packet.
	DataSerializer writer(data->buffer.buf, data->buffer.len);

	const packet_id_t packetID = datagramPacketIDCounter;
	writer.Add<u8>((u8)((packetID & 63) | ((reliable ? 1 : 0) << 6)  | ((inOrder ? 1 : 0) << 7)));
	writer.Add<u16>((u16)(packetID >> 6));
	if (reliable)
	{
		assert((smallestReliableMessageNumber & 0x80000000) == 0);
		writer.AddVLE<VLE16_32>(smallestReliableMessageNumber);
	}

	bool sentDisconnectAckMessage = false;

	// Write all the messages in this UDP packet.
	for(size_t i = 0; i < datagramSerializedMessages.size(); ++i)
	{
		NetworkMessage *msg = datagramSerializedMessages[i];
		assert(!msg->transfer || msg->transfer->id != -1);

		const int encodedMsgIdLength = (msg->transfer == 0 || msg->fragmentIndex == 0) ? VLE8_16_32::GetEncodedBitLength(msg->id)/8 : 0;
		const size_t messageContentSize = msg->dataSize + encodedMsgIdLength; // 1/2/4 bytes: Message ID. X bytes: Content.
		assert(messageContentSize < (1 << 11));

		if (msg->id == MsgIdDisconnectAck)
			sentDisconnectAckMessage = true;

		const u16 reliable = (msg->reliable ? 1 : 0) << 12;
		const u16 inOrder = (msg->inOrder ? 1 : 0) << 13;
		const u16 fragmentedTransfer = (msg->transfer != 0 ? 1 : 0) << 14;
		const u16 firstFragment = (msg->transfer != 0 && msg->fragmentIndex == 0 ? 1 : 0) << 15;
		writer.Add<u16>((u16)messageContentSize | reliable | inOrder | fragmentedTransfer | firstFragment);

		if (msg->reliable)
			writer.AddVLE<VLE8_16>(msg->reliableMessageNumber - smallestReliableMessageNumber);

		///\todo Add the InOrder index here to track which datagram/message we depended on.

		assert((!firstFragment && !fragmentedTransfer) || msg->transfer);

		if (firstFragment != 0)
			writer.AddVLE<VLE8_16_32>(msg->transfer->totalNumFragments);
		if (fragmentedTransfer != 0)
			writer.Add<u8>((u8)msg->transfer->id);
		if (firstFragment == 0 && fragmentedTransfer != 0)
			writer.AddVLE<VLE8_16_32>(msg->fragmentIndex); // The message fragment number.
		if (msg->transfer == 0 || msg->fragmentIndex == 0)
			writer.AddVLE<VLE8_16_32>(msg->id); // Add the message ID number.
		if (msg->dataSize > 0) // Add the actual message payload data.
			writer.AddAlignedByteArray(msg->data, msg->dataSize);
	}

	// Send the crafted packet out to the socket.
	data->buffer.len = writer.BytesFilled();
	bool success = socket->EndSend(data);

	if (!success)
	{
		// We failed, so put all messages back to the outbound queue, except for those that are from old in-order packet,
		// since they need to be resent with the old packet ID and not as fresh messages.
		for(size_t i = 0; i < datagramSerializedMessages.size(); ++i)
			outboundQueue.Insert(datagramSerializedMessages[i]);

		LOGNET("Socket::Send failed to socket %s!", socket->ToString().c_str());
		return PacketSendSocketFull;
	}

	// Sending the datagram succeeded - increment the send count of each message by one, to remember the retry timeout count.
	for(size_t i = 0; i < datagramSerializedMessages.size(); ++i)
		++datagramSerializedMessages[i]->sendCount;

	assert(socket->TransportLayer() == SocketOverUDP);

	// Now we have to wait 1/datagramSendRate seconds again until we can send the next datagram.
	NewDatagramSent();

	// The send was successful, we can increment our next free PacketID counter to use for the next packet.
	lastSentInOrderPacketID = datagramPacketIDCounter;
	datagramPacketIDCounter = AddPacketID(datagramPacketIDCounter, 1);

	AddOutboundStats(writer.BytesFilled(), 1, datagramSerializedMessages.size());

	if (reliable)
	{
		// Now that we have sent a reliable datagram, remember all messages that were
		// serialized into this datagram so that we can properly resend the messages in the datagram if it times out.
		PacketAckTrack ack;
		ack.packetID = packetID;
		const tick_t now = Clock::Tick();
		ack.sendCount = 1;
		ack.sentTick = now;
		ack.timeoutTick = now + (tick_t)((double)retransmissionTimeout * Clock::TicksPerMillisecond());
		ack.datagramSendRate = datagramSendRate;

		for(size_t i = 0; i < datagramSerializedMessages.size(); ++i)
		{
			if (datagramSerializedMessages[i]->reliable)
				ack.messages.push_back(datagramSerializedMessages[i]); // The ownership of these messages is transferred into this struct.
			else
				FreeMessage(datagramSerializedMessages[i]);
		}
		outboundPacketAckTrack.Insert(ack);
	}
	else // We sent an unreliable datagram.
	{
		// This is send-and-forget, we can free all the message data we just sent.
		for(size_t i = 0; i < datagramSerializedMessages.size(); ++i)
			FreeMessage(datagramSerializedMessages[i]);
	}

	// If we sent out the DisconnectAck message, we can close down the connection right now.
	if (sentDisconnectAckMessage)
	{
		connectionState = ConnectionClosed;
		LOGNET("Connection closed by peer: %s.", ToString().c_str());
	}

	return PacketSendOK;
}

void UDPMessageConnection::DoUpdateConnection()
{
	if (udpUpdateTimer.TriggeredOrNotRunning())
	{
		// We can send out data now. Perform connection management before sending out any messages.
		ProcessPacketTimeouts();
		HandleFlowControl();

		// Generate an Ack message if we've accumulated enough reliable messages to make it
		// worthwhile or if some of them are timing out.
		PerformPacketAckSends();

		udpUpdateTimer.StartMSecs(10.f);
	}

/*
	if (statsUpdateTimer.TriggeredOrNotRunning())
	{
		///\todo Put this behind a timer - update only once every 1 sec or so.
		ComputePacketLoss();
		statsUpdateTimer.StartMSecs(1000.f);
	}
*/
}

unsigned long UDPMessageConnection::TimeUntilCanSendPacket() const
{
	tick_t now = Clock::Tick();

	if (Clock::IsNewer(now, lastDatagramSendTime))
		return 0;

	if (Clock::IsNewer(lastDatagramSendTime, now + Clock::TicksPerSec()))
		lastDatagramSendTime = now + Clock::TicksPerSec();

	return (unsigned long)Clock::TimespanToMillisecondsF(now, lastDatagramSendTime);
}

bool UDPMessageConnection::HaveReceivedPacketID(packet_id_t packetID) const
{
	return receivedPacketIDs.Exists(packetID);
}

void UDPMessageConnection::AddReceivedPacketIDStats(packet_id_t packetID)
{
/* \todo add back to enable packet loss compuptations.
	ConnectionStatistics &cs = stats.Lock();

	// Simple method to prevent computation errors caused by wraparound - we start from scratch when packet with ID 0 is received.
//	if (packetID == 0)
//		cs.recvPacketIDs.clear();

	cs.recvPacketIDs.push_back(ConnectionStatistics::DatagramIDTrack());
	ConnectionStatistics::DatagramIDTrack &t = cs.recvPacketIDs.back();
	t.tick = Clock::Tick();
	t.packetID = packetID;
//	LOGNET("Marked packet with ID %d received.", (unsigned long)packetID);
	stats.Unlock();
*/
	// Remember this packet ID for duplicacy detection and pruning purposes.
	receivedPacketIDs.Add(packetID);
}

void UDPMessageConnection::ExtractMessages(const char *data, size_t numBytes)
{
	assert(data);
	assert(numBytes > 0);

	// Immediately discard this datagram if it might contain more messages than we can handle. Otherwise
	// we might end up in a situation where we have already applied some of the messages in the datagram
	// and realize we don't have space to take in the rest, which would require a "partial ack" of sorts.
	if (inboundMessageQueue.CapacityLeft() < 64)
		return;

	lastHeardTime = Clock::Tick();

	if (numBytes < 3)
	{
		LOGNET("Malformed UDP packet when reading packet header! Size = %d bytes, no space for packet header, which is at least 3 bytes.", numBytes);
		return;
	}

	DataDeserializer reader(data, numBytes);

	// Start by reading the packet header (flags, packetID).
	u8 flags = reader.Read<u8>();
	bool inOrder = (flags & (1 << 7)) != 0;
	bool packetReliable = (flags & (1 << 6)) != 0;
	packet_id_t packetID = (reader.Read<u16>() << 6) | (flags & 63);

	unsigned long reliableMessageIndexBase = (packetReliable ? reader.ReadVLE<VLE16_32>() : 0); ///\todo sanitize input length.

	// If the 'reliable'-flag is set, remember this PacketID, we need to Ack it later on.
	if (packetReliable)
	{
		PacketAckTrack &t = inboundPacketAckTrack[packetID];
		t.packetID = packetID;
		// The following are not used right now.
		///\todo If we want to queue up a few acks before sending an ack message, we should possibly save here
		// the time when we received the packet.
		t.sentTick = Clock::Tick();
	}

	// Note that this check must be after the ack check (above), since we still need to ack the new packet as well (our
	// previous ack might not have reached the sender or was delayed, which is why he's resending it).
	if (HaveReceivedPacketID(packetID))
		return;

	// If the 'inOrder'-flag is set, there's an extra 'Order delta counter' field present,
	// that specifies the processing ordering of this packet.
	packet_id_t inOrderID = 0;
	if (inOrder)
	{

//		inOrderID = reader.ReadVLE<VLE8_16>();
		if (inOrderID == DataDeserializer::VLEReadError)
		{
			LOGNET("Malformed UDP packet! Size = %d bytes, no space for packet header field 'inOrder'!", numBytes);
			return;
		}
	}

	size_t numMessagesReceived = 0;
	while(reader.BytesLeft() > 0)
	{
		if (reader.BytesLeft() < 2)
		{
			LOGNET("Malformed UDP packet! Parsed %d messages ok, but after that there's not enough space for UDP message header! BytePos %d, total size %d",
				reader.BytePos(), numBytes);
			return;
		}

		// Read the message header (2 bytes at least).
		u16 contentLength = reader.Read<u16>();
		bool fragmentStart = (contentLength & (1 << 15)) != 0;
		bool fragment = (contentLength & (1 << 14)) != 0 || fragmentStart; // If fragmentStart is set, then fragment is set.
		bool inOrder = (contentLength & (1 << 13)) != 0;
		bool messageReliable = (contentLength & (1 << 12)) != 0;
		contentLength &= (1 << 11) - 1;

		// If true, this message is a duplicate one we've received, and will be discarded. We need to parse it fully though,
		// to be able to parse the messages that come after it.
		bool duplicateMessage = false; 

		unsigned long reliableMessageNumber = 0;
		if (messageReliable)
		{
			reliableMessageNumber = reliableMessageIndexBase + reader.ReadVLE<VLE8_16>();

			if (receivedReliableMessages.find(reliableMessageNumber) != receivedReliableMessages.end())
				duplicateMessage = true;
			else 
				receivedReliableMessages.insert(reliableMessageNumber);
		}

		if (contentLength == 0)
		{
			LOGNET("Malformed UDP packet! Byteofs %d, Packet length %d. Message had zero length (Length must be at least one byte)!", reader.BytePos(), numBytes);
			return;
		}

		u32 numTotalFragments = (fragmentStart ? reader.ReadVLE<VLE8_16_32>() : 0);
		u8 fragmentTransferID = (fragment ? reader.Read<u8>() : 0);
		u32 fragmentNumber = (fragment && !fragmentStart ? reader.ReadVLE<VLE8_16_32>() : 0);

		if (reader.BytesLeft() < contentLength)
		{
			LOGNET("Malformed UDP packet! Byteofs %d, Packet length %d. Expected %d bytes of message content, but only %d bytes left!",
				reader.BytePos(), numBytes, contentLength, reader.BytesLeft());
			return;
		}

		// If we received the start of a new fragment, start tracking a new fragmented transfer.
		if (fragmentStart)
		{
			if (numTotalFragments == DataDeserializer::VLEReadError || numTotalFragments <= 1)
			{
				LOGNET("Malformed UDP packet! This packet had fragmentStart bit on, but parsing numTotalFragments VLE failed!");
				return;
			}

			if (!duplicateMessage)
				fragmentedReceives.NewFragmentStartReceived(fragmentTransferID, numTotalFragments, &data[reader.BytePos()], contentLength);
		}
		// If we received a fragment that is a part of an old fragmented transfer, pass it to the fragmented transfer manager
		// so that it can reconstruct the final stream when the transfer finishes.
		else if (fragment)
		{
			if (fragmentNumber == DataDeserializer::VLEReadError)
			{
				LOGNET("Malformed UDP packet! This packet has fragment flag on, but parsing the fragment number failed!");
				return;
			}

			bool messageReady = fragmentedReceives.NewFragmentReceived(fragmentTransferID, fragmentNumber, &data[reader.BytePos()], contentLength);
			if (messageReady)
			{
				// This was the last fragment of the whole message - reconstruct the message from the fragments and pass it on to
				// the client to handle.
				assembledData.clear();
				fragmentedReceives.AssembleMessage(fragmentTransferID, assembledData);
				assert(assembledData.size() > 0);
				///\todo InOrder.
				HandleInboundMessage(packetID, &assembledData[0], assembledData.size());
				++numMessagesReceived;
				fragmentedReceives.FreeMessage(fragmentTransferID);
			}
		}
		else if (!duplicateMessage)
		{
			// Not a fragment, so directly call the handling code.
			HandleInboundMessage(packetID, &data[reader.BytePos()], contentLength);
			++numMessagesReceived;
		}

		reader.SkipBytes(contentLength);
	}

	// Store the packetID for inbound packet loss statistics purposes.
	AddReceivedPacketIDStats(packetID);
	// Save general statistics (bytes, packets, messages rate).
	AddInboundStats(0, 1, numMessagesReceived);
}

void UDPMessageConnection::PerformDisconnection()
{
	SendDisconnectMessage(false);
}

bool UDPMessageConnection::CanSendOutNewDatagram() const
{
	const tick_t now = Clock::Tick();

	const tick_t datagramSendTickDelay = (tick_t)(Clock::TicksPerSec() / datagramSendRate);

	return Clock::TicksInBetween(now, lastDatagramSendTime) >= datagramSendTickDelay;
}

void UDPMessageConnection::NewDatagramSent()
{
	const tick_t datagramSendTickDelay = (tick_t)(Clock::TicksPerSec() / datagramSendRate);
	const tick_t now = Clock::Tick();

	if (Clock::TicksInBetween(now, lastDatagramSendTime) / datagramSendTickDelay < 20)
		lastDatagramSendTime += datagramSendTickDelay;
	else
		lastDatagramSendTime = now;
}

void UDPMessageConnection::SendDisconnectMessage(bool isInternal)
{
	NetworkMessage *msg = StartNewMessage(MsgIdDisconnect);
	msg->priority = NetworkMessage::cMaxPriority; ///\todo Highest or lowest priority depending on whether to finish all pending messages?
	msg->reliable = true;
	EndAndQueueMessage(msg, isInternal);
}

void UDPMessageConnection::SendDisconnectAckMessage()
{
	NetworkMessage *msg = StartNewMessage(MsgIdDisconnectAck);
	msg->priority = NetworkMessage::cMaxPriority; ///\todo Highest or lowest priority depending on whether to finish all pending messages?
	msg->reliable = false;
	EndAndQueueMessage(msg, true); ///\todo Check this flag!
}

void UDPMessageConnection::HandleFlowControlRequestMessage(const char *data, size_t numBytes)
{/*
	if (numBytes != 2)
	{
		LOGNET("Malformed FlowControlRequest message received! Size was %d bytes, expected 2 bytes!", numBytes);
		return;
	}

	const u16 minOutboundRate = 5;
	const u16 maxOutboundRate = 10 * 1024;
	u16 newOutboundRate = *reinterpret_cast<const u16*>(data);
	if (newOutboundRate < minOutboundRate || newOutboundRate > maxOutboundRate)
	{
		LOGNET("Invalid FlowControlRequest rate %d packets/sec received! Ignored. Valid range (%d, %d)", newOutboundRate,
			minOutboundRate, maxOutboundRate);
		return;
	}

//	LOGNET("Received FlowControl message. Adjusting OutRate from %d to %d msgs/sec.", datagramOutRatePerSecond, newOutboundRate);

	datagramOutRatePerSecond = newOutboundRate;*/
}

int UDPMessageConnection::BiasedBinarySearchFindPacketIndex(PacketAckTrackQueue &queue, int packetID)
{
	///\bug Make this all packetID wrap-around -aware.

	int headIdx = 0;
	PacketAckTrack *headItem = queue.ItemAt(headIdx);
	if (headItem->packetID == packetID)
		return headIdx;
	int tailIdx = queue.Size()-1;
	PacketAckTrack *tailItem = queue.ItemAt(tailIdx);
	if (tailItem->packetID == packetID)
		return tailIdx;
	assert(headItem->packetID < tailItem->packetID);
	if ((int)headItem->packetID > packetID || (int)tailItem->packetID < packetID)
		return -1;
	while(headIdx < tailIdx)
	{
		int newIdx = (tailIdx - headIdx) * (packetID - headItem->packetID) / (tailItem->packetID - headItem->packetID);
		newIdx = max(headIdx+1, min(tailIdx-1, newIdx));
		PacketAckTrack *newItem = queue.ItemAt(newIdx);
		if (newItem->packetID == packetID)
			return newIdx;
		else if ((int)newItem->packetID < packetID)
		{
			headIdx = newIdx;
			headItem = newItem;
		}
		else
		{
			tailIdx = newIdx;
			tailItem = newItem;
		}
	}
	return -1;
}

void UDPMessageConnection::FreeOutboundPacketAckTrack(packet_id_t packetID)
{
	OrderedHashTable<PacketAckTrack, PacketAckTrack>::Node *item = outboundPacketAckTrack.Find(packetID);
	if (!item)
		return;

	// Free up all the messages in the acked packet. We don't need to keep track of those any more (to be sent to peer).
	PacketAckTrack &track = item->value;
	for(size_t i = 0; i < track.messages.size(); ++i)
	{
		// If the message was part of a fragmented transfer, remove the message from that data structure.
		if (track.messages[i]->transfer)
		{
			Lock<FragmentedSendManager> sends = fragmentedSends.Acquire();
			sends->RemoveMessage(track.messages[i]->transfer, track.messages[i]);
		}

		// Free up the message, the peer acked this message and we're now free from having to resend it (again).
		FreeMessage(track.messages[i]);
	}

	if (track.sendCount <= 1)
	{
		UpdateRTOCounterOnPacketAck((float)Clock::TimespanToSecondsD(track.sentTick, Clock::Tick()));
		++numAcksLastFrame;
	}

	outboundPacketAckTrack.Remove(packetID);
}

static const float minRTOTimeoutValue = 1000.f;
static const float maxRTOTimeoutValue = 5000.f;

/// Adjusts the retransmission timer values as per RFC 2988.
/// @param rtt The round trip time that was measured on the packet that was just acked.
void UDPMessageConnection::UpdateRTOCounterOnPacketAck(float rtt)
{
	using namespace std;

	const float alpha = 1.f / 8.f;
	const float beta = 1.f / 4.f;

	if (rttCleared)
	{
		rttCleared = false;
		rttVariation = rtt / 2.f;
		smoothedRTT = rtt;
	}
	else
	{
		rttVariation = (1.f - beta) * rttVariation + beta * fabs(smoothedRTT - rtt);
		smoothedRTT = (1.f - alpha) * smoothedRTT + alpha * rtt;
	}
	// We add this much constant delay to all RTO timers to avoid too optimistic RTO values
	// in excellent conditions (localhost, LAN).
	const float safetyThresholdAdd = 1.f;
	const float safetyThresholdMul = 2.f;

	retransmissionTimeout = min(maxRTOTimeoutValue, max(minRTOTimeoutValue, safetyThresholdAdd + safetyThresholdMul * (smoothedRTT + rttVariation)));

///	const float maxDatagramSendRate = 3000.f;
	// Update data send rate.
//	++datagramOutRatePerSecond; // Additive increases.
//	datagramSendRate = datagramSendRate + 1.f; // Increase by one datagram/successfully sent packet.
//	datagramSendRate = min(datagramSendRate + 1.f, maxDatagramSendRate); // Increase by one datagram/successfully sent packet.

//	LOGNET("Packet ack event: RTO: %.3f sec., srtt: %.3f sec., rttvar: %.3f sec. datagramSendRate: %.2f", 
//		retransmissionTimeout, smoothedRTT, rttVariation, datagramSendRate);
}

void UDPMessageConnection::UpdateRTOCounterOnPacketLoss()
{
	using namespace std;

	retransmissionTimeout = smoothedRTT = min(maxRTOTimeoutValue, max(minRTOTimeoutValue, smoothedRTT * 2.f));
	// The variation just gives bogus values, so clear it altogether.
	rttVariation = 0.f;

	// Multiplicative decreases.
//	datagramOutRatePerSecond = max(1, datagramOutRatePerSecond / 2);
//	datagramSendRate = max(1.f, datagramSendRate * 0.9f); // At least send one packet/second.

	++numLossesLastFrame;

//	LOGNET("Packet loss event: RTO: %.3f sec. datagramSendRate: %.2f", retransmissionTimeout, datagramSendRate);
}

void UDPMessageConnection::SendPacketAckMessage()
{
	while(inboundPacketAckTrack.size() > 0)
	{
		packet_id_t packetID = inboundPacketAckTrack.begin()->first;
		u32 sequence = 0;

		inboundPacketAckTrack.erase(packetID);
		for(int i = 0; i < 32; ++i)
		{
			packet_id_t id = AddPacketID(packetID, i + 1);
			
			PacketAckTrackMap::iterator iter = inboundPacketAckTrack.find(id);
			if (iter != inboundPacketAckTrack.end())
			{
				sequence |= 1 << i;
				inboundPacketAckTrack.erase(id);
			}
		}

		NetworkMessage *msg = StartNewMessage(MsgIdPacketAck, 7);
		DataSerializer mb(msg->data, 7);
		mb.Add<u8>((u8)(packetID & 0xFF));
		mb.Add<u16>((u16)(packetID >> 8));
		mb.Add<u32>(sequence);
		msg->priority = NetworkMessage::cMaxPriority - 1;
		EndAndQueueMessage(msg, mb.BytesFilled(), true);
	}
}

void UDPMessageConnection::HandlePacketAckMessage(const char *data, size_t numBytes)
{
	if (numBytes != 7)
	{
		LOGNET("Malformed PacketAck message received! Size was %d bytes, expected 7 bytes!", numBytes);
		return;
	}

	DataDeserializer mr(data, numBytes);
	packet_id_t packetIDLow = (packet_id_t)mr.Read<u8>();
	packet_id_t packetIDHigh = (packet_id_t)mr.Read<u16>();
	packet_id_t packetID = packetIDLow | (packetIDHigh << 8);
	u32 sequence = mr.Read<u32>();

	FreeOutboundPacketAckTrack(packetID);
	for(size_t i = 0; i < 32; ++i)
		if ((sequence & (1 << i)) != 0)
		{
			packet_id_t id = AddPacketID(packetID, 1 + i);
			FreeOutboundPacketAckTrack(id);
		}
}

void UDPMessageConnection::HandleDisconnectMessage()
{
	if (connectionState != ConnectionClosed)
	{
		connectionState = ConnectionDisconnecting;
		SendDisconnectAckMessage();
	}
}

void UDPMessageConnection::HandleDisconnectAckMessage()
{
	if (connectionState != ConnectionDisconnecting)
		LOGNET("Received DisconnectAck message on a MessageConnection not in ConnectionDisconnecting state! (state was %d)",
		connectionState);
	else
		LOGNET("Connection closed to %s.", ToString().c_str());

	connectionState = ConnectionClosed;
}

void UDPMessageConnection::PerformFlowControl()
{
	/*
	// The manual flow control only applies to UDP connections.
	if (socket->TransportLayer() == SocketOverTCP)
		return;

	const float maxAllowedPacketLossRate = 0.f;
	if (GetPacketLossRate() > maxAllowedPacketLossRate)
	{
		float newInboundRate = PacketsInPerSec() * (1.f - GetPacketLossRate());
//		LOGNET("Packet loss rate: %.2f. Adjusting InRate from %d to %d!", GetPacketLossRate(), datagramInRatePerSecond, (int)newInboundRate);
		SetDatagramInFlowRatePerSecond((int)newInboundRate, true);
	}
	else if (PacketsInPerSec() >= (float)datagramInRatePerSecond / 2)
	{
		const int flowRateIncr = 50;
//		LOGNET("Have received %.2f packets in/sec with loss rate of %.2f. Increasing InRate from %d to %d.",
//			PacketsInPerSec(), GetPacketLossRate(), datagramInRatePerSecond, datagramInRatePerSecond + flowRateIncr);
		SetDatagramInFlowRatePerSecond(datagramInRatePerSecond + flowRateIncr, true);
	}
	*/
}

void UDPMessageConnection::ComputePacketLoss()
{
	Lockable<ConnectionStatistics>::LockType cs = stats.Acquire();

	if (cs->recvPacketIDs.size() <= 1)
	{
		packetLossRate = packetLossCount = 0.f;
		return;
	}

	const tick_t maxEntryAge = Clock::TicksPerSec() * 5;
	const tick_t timeNow = Clock::Tick();
	const tick_t maxTickAge = timeNow - maxEntryAge;

	// Remove old entries.
	for(size_t i = 0; i < cs->recvPacketIDs.size(); ++i)
		if (Clock::IsNewer(cs->recvPacketIDs[i].tick, maxTickAge))
		{
			cs->recvPacketIDs.erase(cs->recvPacketIDs.begin(), cs->recvPacketIDs.begin() + i);
			break;
		}

	if (cs->recvPacketIDs.size() <= 1)
	{
		packetLossRate = packetLossCount = 0.f;
		return;
	}

	// Find the oldest packet (in terms of messageID)
	int oldestIndex = 0;
	for(size_t i = 1; i < cs->recvPacketIDs.size(); ++i)
		if (PacketIDIsNewerThan(cs->recvPacketIDs[oldestIndex].packetID, cs->recvPacketIDs[i].packetID))
			oldestIndex = i;

	std::vector<packet_id_t> relIDs;
	relIDs.reserve(cs->recvPacketIDs.size());
	for(size_t i = 0; i < cs->recvPacketIDs.size(); ++i)
		relIDs.push_back(SubPacketID(cs->recvPacketIDs[i].packetID, cs->recvPacketIDs[oldestIndex].packetID));

	sort::CocktailSort(&relIDs[0], relIDs.size());

	int numMissedPackets = 0;
	for(size_t i = 0; i+1 < cs->recvPacketIDs.size(); ++i)
	{
		assert(relIDs[i+1] > relIDs[i]);
		numMissedPackets += relIDs[i+1] - relIDs[i] - 1;
	}

	packetLossRate = (float)numMissedPackets / (cs->recvPacketIDs.size() + numMissedPackets);
	packetLossCount = (float)numMissedPackets * 1000.f / (float)Clock::TimespanToMillisecondsD(maxTickAge, timeNow);
}

void AppendU16ToVector(std::vector<char> &data, unsigned long value)
{
	data.insert(data.end(), (const char *)&value, (const char *)&value + 2);
}

void UDPMessageConnection::SetDatagramInFlowRatePerSecond(int newDatagramReceiveRate, bool internalCall)
{/*
	if (newDatagramReceiveRate == datagramInRatePerSecond) // No need to set it multiple times.
		return;

	if (newDatagramReceiveRate < 5 || newDatagramReceiveRate > 10 * 1024)
	{
		LOGNET("Tried to set invalid UDP receive rate %d packets/sec! Ignored.", newDatagramReceiveRate);
		return;
	}
	
	datagramInRatePerSecond = newDatagramReceiveRate;

	NetworkMessage &msg = StartNewMessage(MsgIdFlowControlRequest);
	AppendU16ToVector(msg.data, newDatagramReceiveRate);
	msg.priority = NetworkMessage::cMaxPriority - 1;
	EndAndQueueMessage(msg, internalCall);*/
}

bool UDPMessageConnection::HandleMessage(packet_id_t packetID, u32 messageID, const char *data, size_t numBytes)
{
	switch(messageID)
	{
	case MsgIdPingRequest:
	case MsgIdPingReply:
		return false; // We don't do anything with these messages, the MessageConnection base class handles these.

	case MsgIdFlowControlRequest:
		HandleFlowControlRequestMessage(data, numBytes);
		return true;
	case MsgIdPacketAck:
		HandlePacketAckMessage(data, numBytes);
		return true;
	case MsgIdDisconnect:
		HandleDisconnectMessage();
		return true;
	case MsgIdDisconnectAck:
		HandleDisconnectAckMessage();
		return true;
	default:
		if (!inboundMessageHandler)
			return false;
		else
		{
			u32 contentID = inboundMessageHandler->ComputeContentID(messageID, data, numBytes);
			if (contentID != 0 && CheckAndSaveContentIDStamp(messageID, contentID, packetID) == false)
			{
				LOGNETVERBOSE("MessageID %u in packetID %d and contentID %u is obsolete! Skipped.", messageID, (int)packetID, contentID);
				return true;
			}
			return false;
		}
	}
}

void UDPMessageConnection::DumpConnectionStatus() const
{
	char str[2048];
	sprintf(str,
		"\tRetransmission timeout: %.2fms.\n"
		"\tDatagram send rate: %.2f/sec.\n"
		"\tSmoothed RTT: %.2fms.\n"
		"\tRTT variation: %.2f.\n"
		"\tOutbound reliable datagrams in flight: %d.\n"
		"\tReceived unacked datagrams: %d.\n"
		"\tPacket loss count: %.2f.\n"
		"\tPacket loss rate: %.2f.\n"
		"\tDatagrams in: %.2f/sec.\n"
		"\tDatagrams out: %.2f/sec.\n",
	retransmissionTimeout,
	datagramSendRate,
	smoothedRTT,
	rttVariation,
	outboundPacketAckTrack.Size(), ///\todo Accessing this variable is not thread-safe.
	inboundPacketAckTrack.size(), ///\todo Accessing this variable is not thread-safe.
	packetLossCount,
	packetLossRate,
	PacketsInPerSec(), 
	PacketsOutPerSec());

	LOGUSER(str);
}

} // ~kNet