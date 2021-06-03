#include "Limelight-internal.h"

#ifdef LC_DEBUG
// This enables FEC validation mode with a synthetic drop
// and recovered packet checks vs the original input. It
// is on by default for debug builds.
//
// NB: Unlike the video FEC feature of the same name, this
// is much more restrictive in terms of when the validation
// runs. Due to the logic to immediately return in-order
// data packets, it requires non-consecutive data packets to
// trigger the call to completeFecBlock(). Missing or OOO
// packets will do the job.
#define FEC_VALIDATION_MODE
#endif

void RtpaInitializeQueue(PRTP_AUDIO_QUEUE queue) {
    memset(queue, 0, sizeof(*queue));
    queue->maxQueueTimeMs = RTPQ_DEFAULT_QUEUE_TIME;
    queue->nextRtpSequenceNumber = UINT16_MAX;

    reed_solomon_init();

    // The number of data and parity shards is constant, so we can reuse
    // the same RS matrices for all traffic.
    queue->rs = reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS);

    // For unknown reasons, the RS parity matrix computed by our RS implementation
    // doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
    // but we can simply replace it with the matrix generated by OpenFEC which
    // works correctly. This is possible because the data and FEC shard count is
    // constant and known in advance.
    const unsigned char parity[] = { 0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c };
    memcpy(&queue->rs->m[16], parity, sizeof(parity));
    memcpy(queue->rs->parity, parity, sizeof(parity));
}

static void validateFecBlockState(PRTP_AUDIO_QUEUE queue) {
#ifdef LC_DEBUG
    PRTPA_FEC_BLOCK lastBlock = queue->blockHead;

    // The next sequence number must not be less than the oldest BSN unless we're in the
    // starting state (prior to us setting nextRtpSequenceNumber and oldestRtpBaseSequenceNumber).
    LC_ASSERT(!isBefore16(queue->nextRtpSequenceNumber, queue->oldestRtpBaseSequenceNumber) ||
              (queue->nextRtpSequenceNumber == UINT16_MAX && queue->oldestRtpBaseSequenceNumber == 0));

    if (lastBlock == NULL) {
        return;
    }

    uint16_t lastSeqNum = lastBlock->fecHeader.baseSequenceNumber;
    uint32_t lastTs = lastBlock->fecHeader.baseTimestamp;

    // The head should not have a previous entry
    LC_ASSERT(lastBlock->prev == NULL);

    // The next sequence number must not exceed the first FEC block (otherwise it should have been dequeued and freed)
    LC_ASSERT(isBefore16(queue->nextRtpSequenceNumber, queue->blockHead->fecHeader.baseSequenceNumber + RTPA_DATA_SHARDS));

    // The first FEC block should not be before the oldest BSN (or we will drop packets that belong in that FEC block).
    LC_ASSERT(!isBefore16(queue->blockHead->fecHeader.baseSequenceNumber, queue->oldestRtpBaseSequenceNumber));

    PRTPA_FEC_BLOCK block = lastBlock->next;
    while (block != NULL) {
        // Ensure the list is sorted correctly
        LC_ASSERT(isBefore16(lastSeqNum, block->fecHeader.baseSequenceNumber));
        LC_ASSERT(isBefore32(lastTs, block->fecHeader.baseTimestamp));

        // Ensure entry invariants are satisfied
        LC_ASSERT(block->blockSize == lastBlock->blockSize);
        LC_ASSERT(block->fecHeader.payloadType == lastBlock->fecHeader.payloadType);
        LC_ASSERT(block->fecHeader.ssrc == lastBlock->fecHeader.ssrc);

        // Ensure the list itself is consistent
        LC_ASSERT(block->prev == lastBlock);
        LC_ASSERT(block->next != NULL || queue->blockTail == block);

        lastBlock = block;
        block = block->next;
    }
#endif
}

static void freeFecBlockHead(PRTP_AUDIO_QUEUE queue) {
    PRTPA_FEC_BLOCK blockHead = queue->blockHead;

    queue->blockHead = queue->blockHead->next;
    if (queue->blockHead != NULL) {
        queue->blockHead->prev = NULL;
    }
    else {
        LC_ASSERT(queue->blockTail == blockHead);
        queue->blockTail = NULL;
    }

    queue->oldestRtpBaseSequenceNumber = blockHead->fecHeader.baseSequenceNumber + RTPA_DATA_SHARDS;

    validateFecBlockState(queue);

    free(blockHead);
}

void RtpaCleanupQueue(PRTP_AUDIO_QUEUE queue) {
    while (queue->blockHead != NULL) {
        freeFecBlockHead(queue);
    }

    LC_ASSERT(queue->blockTail == NULL);

    reed_solomon_release(queue->rs);
    queue->rs = NULL;
}

static PRTPA_FEC_BLOCK getFecBlockForRtpPacket(PRTP_AUDIO_QUEUE queue, PRTP_PACKET packet, uint16_t length) {
    uint32_t fecBlockSsrc;
    uint16_t fecBlockBaseSeqNum;
    uint32_t fecBlockBaseTs;
    uint16_t blockSize;
    uint8_t fecBlockPayloadType;

    validateFecBlockState(queue);

    if (packet->packetType == 97) {
        if (length < sizeof(RTP_PACKET)) {
            Limelog("RTP audio data packet too small: %u\n", length);
            LC_ASSERT(false);
            return NULL;
        }

        // This is a data packet, so we will need to synthesize an FEC header
        fecBlockPayloadType = packet->packetType;
        fecBlockBaseSeqNum = (packet->sequenceNumber / RTPA_DATA_SHARDS) * RTPA_DATA_SHARDS;
        fecBlockBaseTs = packet->timestamp - ((packet->sequenceNumber - fecBlockBaseSeqNum) * AudioPacketDuration);
        fecBlockSsrc = packet->ssrc;

        blockSize = length - sizeof(RTP_PACKET);
    }
    else if (packet->packetType == 127) {
        PAUDIO_FEC_HEADER fecHeader = (PAUDIO_FEC_HEADER)(packet + 1);

        if (length < sizeof(RTP_PACKET) + sizeof(AUDIO_FEC_HEADER)) {
            Limelog("RTP audio FEC packet too small: %u\n", length);
            LC_ASSERT(false);
            return NULL;
        }

        // This is an FEC packet, so we can just copy (and byteswap) the FEC header
        fecBlockPayloadType = fecHeader->payloadType;
        fecBlockBaseSeqNum = BE16(fecHeader->baseSequenceNumber);
        fecBlockBaseTs = BE32(fecHeader->baseTimestamp);
        fecBlockSsrc = BE32(fecHeader->ssrc);

        // Ensure the FEC shard index is valid to prevent OOB access
        // later during recovery.
        if (fecHeader->fecShardIndex >= RTPA_FEC_SHARDS) {
            Limelog("Too many audio FEC shards: %u\n", fecHeader->fecShardIndex);
            LC_ASSERT(false);
            return NULL;
        }

        blockSize = length - sizeof(RTP_PACKET) - sizeof(AUDIO_FEC_HEADER);
    }
    else {
        LC_ASSERT(false);
        return NULL;
    }

    // Drop packets from FEC blocks that have already been completed
    if (isBefore16(fecBlockBaseSeqNum, queue->oldestRtpBaseSequenceNumber)) {
        return NULL;
    }

    // Look for an existing FEC block
    PRTPA_FEC_BLOCK existingBlock = queue->blockHead;
    while (existingBlock != NULL) {
        if (existingBlock->fecHeader.baseSequenceNumber == fecBlockBaseSeqNum) {
            // The FEC header data should match for all packets
            LC_ASSERT(existingBlock->fecHeader.payloadType == fecBlockPayloadType);
            LC_ASSERT(existingBlock->fecHeader.baseTimestamp == fecBlockBaseTs);
            LC_ASSERT(existingBlock->fecHeader.ssrc == fecBlockSsrc);
            LC_ASSERT(existingBlock->blockSize == blockSize);

            // If the block is completed, don't return it
            return existingBlock->fullyReassembled ? NULL : existingBlock;
        }
        else if (isBefore16(fecBlockBaseSeqNum, existingBlock->fecHeader.baseSequenceNumber)) {
            // The new block goes right before this one
            break;
        }

        existingBlock = existingBlock->next;
    }

    // We didn't find an existing FEC block, so we'll have to make one
    uint16_t dataPacketSize = blockSize + sizeof(RTP_PACKET);
    PRTPA_FEC_BLOCK block = malloc(sizeof(*block) + (RTPA_DATA_SHARDS * dataPacketSize) + (RTPA_FEC_SHARDS * blockSize));
    if (block == NULL) {
        return NULL;
    }

    memset(block, 0, sizeof(*block));

    block->queueTimeMs = PltGetMillis();
    block->blockSize = blockSize;
    memset(block->marks, 1, sizeof(block->marks));

    // Set up the FEC header
    block->fecHeader.payloadType = fecBlockPayloadType;
    block->fecHeader.baseSequenceNumber = fecBlockBaseSeqNum;
    block->fecHeader.baseTimestamp = fecBlockBaseTs;
    block->fecHeader.ssrc = fecBlockSsrc;

    // Set up packet buffers pointing into the slab we allocated
    uint8_t* data = (uint8_t*)(block + 1);
    for (int i = 0; i < RTPA_DATA_SHARDS; i++) {
        block->dataPackets[i] = (PRTP_PACKET)data;
        data += dataPacketSize;
    }
    for (int i = 0; i < RTPA_FEC_SHARDS; i++) {
        block->fecPackets[i] = data;
        data += blockSize;
    }

    // Place this block into the list in order
    if (existingBlock != NULL) {
        // This new block comes right before existingBlock
        PRTPA_FEC_BLOCK prevBlock = existingBlock->prev;

        existingBlock->prev = block;

        if (prevBlock == NULL) {
            LC_ASSERT(queue->blockHead == existingBlock);
            queue->blockHead = block;
        }
        else {
            prevBlock->next = block;
        }

        block->prev = prevBlock;
        block->next = existingBlock;
    }
    else {
        // This block goes at the tail of the list
        block->prev = queue->blockTail;
        if (queue->blockTail != NULL) {
            queue->blockTail->next = block;
        }
        queue->blockTail = block;
        if (queue->blockHead == NULL) {
            queue->blockHead = block;
        }
    }

    validateFecBlockState(queue);

    return block;
}

static bool completeFecBlock(PRTP_AUDIO_QUEUE queue, PRTPA_FEC_BLOCK block) {
    uint8_t* shards[RTPA_TOTAL_SHARDS];

    // If we don't have enough shards, we can't do anything.
    // FEC validation mode requires one additional shard.
#ifdef FEC_VALIDATION_MODE
    if (block->dataShardsReceived + block->fecShardsReceived < RTPA_DATA_SHARDS + 1) {
#else
    if (block->dataShardsReceived + block->fecShardsReceived < RTPA_DATA_SHARDS) {
#endif
        return false;
    }

    // If we have all data shards, don't bother with any recovery
    // unless we're in FEC validation mode
    LC_ASSERT(block->dataShardsReceived <= RTPA_DATA_SHARDS);
#ifndef FEC_VALIDATION_MODE
    if (block->dataShardsReceived == RTPA_DATA_SHARDS) {
        return true;
    }
#endif

    // We have recovery to do. Let's build the array.
    for (int i = 0; i < RTPA_DATA_SHARDS; i++) {
        shards[i] = (uint8_t*)(block->dataPackets[i] + 1);
    }
    for (int i = 0; i < RTPA_FEC_SHARDS; i++) {
        shards[RTPA_DATA_SHARDS + i] = block->fecPackets[i];
    }

#ifdef FEC_VALIDATION_MODE
    unsigned int dropIndex;

    // Choose a successfully received packet to drop
    do {
        dropIndex = rand() % RTPA_DATA_SHARDS;
    } while (block->marks[dropIndex]);

    // Copy the original data to validate later
    PRTP_PACKET droppedRtpPacket = malloc(sizeof(RTP_PACKET) + block->blockSize);
    memcpy(droppedRtpPacket, block->dataPackets[dropIndex], sizeof(RTP_PACKET) + block->blockSize);

    // Fake the drop by setting the mark bit and zeroing the "missing" packet
    block->marks[dropIndex] = 1;
    memset(block->dataPackets[dropIndex], 0, sizeof(RTP_PACKET) + block->blockSize);
#endif

    int res = reed_solomon_reconstruct(queue->rs, shards, block->marks, RTPA_TOTAL_SHARDS, block->blockSize);
    if (res != 0) {
        // We should always have enough data to recover the entire block since we checked above.
        LC_ASSERT(res == 0);
        return false;
    }

    // We will need to recover the RTP packet using the FEC header
    for (int i = 0; i < RTPA_DATA_SHARDS; i++) {
        if (block->marks[i]) {
            block->dataPackets[i]->header = 0x80; // RTPv2
            block->dataPackets[i]->packetType = block->fecHeader.payloadType;
            block->dataPackets[i]->sequenceNumber = block->fecHeader.baseSequenceNumber + i;
            block->dataPackets[i]->timestamp = block->fecHeader.baseTimestamp + (i * AudioPacketDuration);
            block->dataPackets[i]->ssrc = block->fecHeader.ssrc;

            block->marks[i] = 0;
        }
    }

#ifdef FEC_VALIDATION_MODE
    // Check the RTP header values
    LC_ASSERT(block->dataPackets[dropIndex]->header == droppedRtpPacket->header);
    LC_ASSERT(block->dataPackets[dropIndex]->packetType == droppedRtpPacket->packetType);
    LC_ASSERT(block->dataPackets[dropIndex]->sequenceNumber == droppedRtpPacket->sequenceNumber);
    LC_ASSERT(block->dataPackets[dropIndex]->timestamp == droppedRtpPacket->timestamp);
    LC_ASSERT(block->dataPackets[dropIndex]->ssrc == droppedRtpPacket->ssrc);

    // Check the data itself - use memcmp() and only loop if an error is detected
    if (memcmp(block->dataPackets[dropIndex] + 1, droppedRtpPacket + 1, block->blockSize)) {
        unsigned char* actualData = (unsigned char*)(block->dataPackets[dropIndex] + 1);
        unsigned char* expectedData = (unsigned char*)(droppedRtpPacket + 1);
        int recoveryErrors = 0;

        for (int j = 0; j < block->blockSize; j++) {
            if (actualData[j] != expectedData[j]) {
                Limelog("Recovery error at %d: expected 0x%02x, actual 0x%02x\n",
                        j, expectedData[j], actualData[j]);
                recoveryErrors++;
            }
        }

        LC_ASSERT(recoveryErrors == 0);
    }

    free(droppedRtpPacket);
#endif

    return true;
}

static bool queueHasPacketReady(PRTP_AUDIO_QUEUE queue) {
    return queue->blockHead != NULL &&
            queue->blockHead->marks[queue->blockHead->nextDataPacketIndex] == 0 &&
            queue->blockHead->fecHeader.baseSequenceNumber + queue->blockHead->nextDataPacketIndex == queue->nextRtpSequenceNumber;
}

static bool enforceQueueConstraints(PRTP_AUDIO_QUEUE queue) {
    // Empty queue is fine
    if (queue->blockHead == NULL) {
        return false;
    }

    // Check that the queue's time constraint is satisfied
    if (PltGetMillis() - queue->blockHead->queueTimeMs > queue->maxQueueTimeMs) {
        Limelog("Unable to recover audio data block %u to %u (%u+%u=%u received < %u needed)\n",
                queue->blockHead->fecHeader.baseSequenceNumber,
                queue->blockHead->fecHeader.baseSequenceNumber + RTPA_DATA_SHARDS - 1,
                queue->blockHead->dataShardsReceived,
                queue->blockHead->fecShardsReceived,
                queue->blockHead->dataShardsReceived + queue->blockHead->fecShardsReceived,
                RTPA_DATA_SHARDS);
        return true;
    }

    return false;
}

int RtpaAddPacket(PRTP_AUDIO_QUEUE queue, PRTP_PACKET packet, uint16_t length) {
    PRTPA_FEC_BLOCK fecBlock = getFecBlockForRtpPacket(queue, packet, length);
    if (fecBlock == NULL) {
        // Reject the packet
        return 0;
    }

    // Synchronize the nextRtpSequenceNumber and oldestRtpBaseSequenceNumber values
    // when the connection begins. We want to always start on FEC block boundaries.
    if (queue->nextRtpSequenceNumber == UINT16_MAX && queue->oldestRtpBaseSequenceNumber == 0) {
        queue->nextRtpSequenceNumber = queue->oldestRtpBaseSequenceNumber = fecBlock->fecHeader.baseSequenceNumber;
        validateFecBlockState(queue);
    }

    if (packet->packetType == 97) {
        uint16_t pos = packet->sequenceNumber - fecBlock->fecHeader.baseSequenceNumber;

        // This is validated in getFecBlockForRtpPacket()
        LC_ASSERT(pos < RTPA_DATA_SHARDS);

        if (fecBlock->marks[pos]) {
            // If there was a missing data shard, copy the RTP header and packet data into it
            memcpy(fecBlock->dataPackets[pos], packet, length);
            fecBlock->marks[pos] = 0;
            fecBlock->dataShardsReceived++;
        }
        else {
            // This is a duplicate packet - reject it
            return 0;
        }

        // This is the common case - an in-order receive of the next data shard.
        // We handle this quickly by telling the caller to immediately consume it.
        if (packet->sequenceNumber == queue->nextRtpSequenceNumber) {
            queue->nextRtpSequenceNumber = packet->sequenceNumber + 1;

            // We are going to return this entry, so update the FEC block
            // state to indicate that the caller has already received it.
            fecBlock->nextDataPacketIndex++;

            // If we've returned all packets in this FEC block, free it.
            if (queue->nextRtpSequenceNumber == U16(fecBlock->fecHeader.baseSequenceNumber + RTPA_DATA_SHARDS)) {
                LC_ASSERT(fecBlock == queue->blockHead);
                LC_ASSERT(fecBlock->nextDataPacketIndex == RTPA_DATA_SHARDS);
                freeFecBlockHead(queue);
            }
            else {
                validateFecBlockState(queue);
            }

            return RTPQ_RET_HANDLE_NOW;
        }
    }
    else if (packet->packetType == 127) {
        PAUDIO_FEC_HEADER fecHeader = (PAUDIO_FEC_HEADER)(packet + 1);

        // This is validated in getFecBlockForRtpPacket()
        LC_ASSERT(fecHeader->fecShardIndex < RTPA_FEC_SHARDS);

        if (fecBlock->marks[RTPA_DATA_SHARDS + fecHeader->fecShardIndex]) {
            // If there was a missing FEC shard, copy just the FEC data into it
            memcpy(fecBlock->fecPackets[fecHeader->fecShardIndex], fecHeader + 1, length - sizeof(RTP_PACKET) - sizeof(AUDIO_FEC_HEADER));
            fecBlock->marks[RTPA_DATA_SHARDS + fecHeader->fecShardIndex] = 0;
            fecBlock->fecShardsReceived++;
        }
        else {
            // This is a duplicate packet - reject it
            return 0;
        }
    }
    else {
        // getFecBlockForRtpPacket() would have already failed
        LC_ASSERT(false);
        return 0;
    }

    // Try to complete the FEC block via data shards or data+FEC shards
    if (completeFecBlock(queue, fecBlock)) {
        // We completed a FEC block
        fecBlock->fullyReassembled = true;
    }

    // The completed FEC block may have readied a packet
    if (queueHasPacketReady(queue)) {
        return RTPQ_RET_PACKET_READY;
    }

    // We don't have enough to proceed. Let's ensure we haven't
    // violated queue constraints with this FEC block.
    if (enforceQueueConstraints(queue)) {
        // Return all available audio data even if there are discontinuities
        queue->blockHead->allowDiscontinuity = true;

        // If the next packet in sequence was in an FEC block that we completely missed,
        // bump the next RTP sequence number to match the beginning of the next block
        // that we received data from.
        //
        // We could avoid setting allowDiscontinuity to see if we can recover the next
        // block. I'm not sure if it makes sense though since we already waited for any
        // packets from the last block. We probably want to get things moving rather than
        // risk waiting a long time again and really starving the audio device.
        if (isBefore16(queue->nextRtpSequenceNumber, queue->blockHead->fecHeader.baseSequenceNumber)) {
            queue->nextRtpSequenceNumber = queue->blockHead->fecHeader.baseSequenceNumber;
        }

        validateFecBlockState(queue);

        return RTPQ_RET_PACKET_READY;
    }

    return queueHasPacketReady(queue) ? RTPQ_RET_PACKET_READY : 0;
}

PRTP_PACKET RtpaGetQueuedPacket(PRTP_AUDIO_QUEUE queue, uint16_t customHeaderLength, uint16_t* length) {
    validateFecBlockState(queue);

    // If we're returning audio data even with discontinuities, find the next data packet
    if (queue->blockHead != NULL && queue->blockHead->allowDiscontinuity) {
        PRTPA_FEC_BLOCK nextBlock = queue->blockHead;

        while (nextBlock->nextDataPacketIndex < RTPA_DATA_SHARDS) {
            LC_ASSERT(nextBlock->fecHeader.baseSequenceNumber + nextBlock->nextDataPacketIndex == queue->nextRtpSequenceNumber);
            if (nextBlock->marks[nextBlock->nextDataPacketIndex]) {
                // This packet is missing. Skip it.
                nextBlock->nextDataPacketIndex++;
                queue->nextRtpSequenceNumber++;
            }
            else {
                LC_ASSERT(queueHasPacketReady(queue));
                break;
            }
        }

        // If we've read everything from this FEC block, remove and free it
        if (nextBlock->nextDataPacketIndex == RTPA_DATA_SHARDS) {
            freeFecBlockHead(queue);
        }
        else {
            validateFecBlockState(queue);
        }
    }

    // Return the next RTP sequence number by indexing into the most recent FEC block
    if (queueHasPacketReady(queue)) {
        PRTPA_FEC_BLOCK nextBlock = queue->blockHead;
        PRTP_PACKET packet = malloc(customHeaderLength + sizeof(RTP_PACKET) + nextBlock->blockSize);
        if (packet == NULL) {
            return NULL;
        }

        *length = nextBlock->blockSize + sizeof(RTP_PACKET);
        memcpy((uint8_t*)packet + customHeaderLength, nextBlock->dataPackets[nextBlock->nextDataPacketIndex], *length);
        nextBlock->nextDataPacketIndex++;

        queue->nextRtpSequenceNumber++;

        // If we've read everything from this FEC block, remove and free it
        if (nextBlock->nextDataPacketIndex == RTPA_DATA_SHARDS) {
            freeFecBlockHead(queue);
        }
        else {
            validateFecBlockState(queue);
        }

        return packet;
    }

    return NULL;
}
