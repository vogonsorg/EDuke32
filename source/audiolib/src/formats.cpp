/*
 Copyright (C) 2009 Jonathon Fowler <jf@jonof.id.au>
 Copyright (C) 2015 EDuke32 developers
 Copyright (C) 2015 Voidpoint, LLC

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

 See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 */

/**
 * Raw, WAV, and VOC source support for MultiVoc
 */

#include "_multivc.h"
#include "compat.h"
#include "multivoc.h"
#include "pitch.h"
#include "pragmas.h"

static playbackstatus MV_GetNextWAVBlock(VoiceNode *voice)
{
    if (voice->BlockLength == 0)
    {
        if (voice->Loop.Start == nullptr)
            return NoMoreData;

        voice->BlockLength = voice->Loop.Size;
        voice->NextBlock   = voice->Loop.Start;
        voice->length      = 0;
        voice->position    = 0;
    }

    voice->sound        = voice->NextBlock;
    voice->position    -= voice->length;
    voice->length       = min(voice->BlockLength, 0x8000u);
    voice->NextBlock   += voice->length * ((voice->channels * voice->bits) >> 3);
    voice->BlockLength -= voice->length;
    voice->length     <<= 16;

    return KeepPlaying;
}

static playbackstatus MV_GetNextVOCBlock(VoiceNode *voice)
{
    size_t   blocklength = 0;
    uint32_t samplespeed = 0;  // XXX: compiler-happy on synthesis
    uint32_t tc          = 0;
    unsigned BitsPerSample;
    unsigned Channels;
    unsigned Format;

    if (voice->BlockLength > 0)
    {
        voice->position    -= voice->length;
        voice->sound       += (voice->length >> 16) * ((voice->channels * voice->bits) >> 3);
        voice->length       = min(voice->BlockLength, 0x8000u);
        voice->BlockLength -= voice->length;
        voice->length     <<= 16;
        return KeepPlaying;
    }

    auto ptr = (uint8_t const *)voice->NextBlock;

    voice->Paused = FALSE;

    int voicemode = 0;
    int blocktype = 0;
    int lastblocktype = 0;
    int packtype = 0;

    int done = FALSE;

    do
    {
        // Stop playing if we get a null pointer
        if (ptr == nullptr)
        {
            done = 2;
            break;
        }

        // terminator is not mandatory according to
        // http://wiki.multimedia.cx/index.php?title=Creative_Voice

        if ((uint32_t)(ptr - (uint8_t *)voice->rawdataptr) >= voice->rawdatasiz)
            blocktype = 0;  // fake a terminator
        else
            blocktype = *ptr;

        if (blocktype != 0)
            blocklength = ptr[1]|(ptr[2]<<8)|(ptr[3]<<16);
        else
            blocklength = 0;
        // would need one byte pad at end of alloc'd region:
//        blocklength = B_LITTLE32(*(uint32_t *)(ptr + 1)) & 0x00ffffff;

        ptr += 4;

        switch (blocktype)
        {
        case 0 :
end_of_data:
            // End of data
            if ((voice->Loop.Start == nullptr) ||
                    ((intptr_t) voice->Loop.Start >= ((intptr_t) ptr - 4)))
            {
                done = 2;
            }
            else
            {
                voice->NextBlock    = voice->Loop.Start;
                voice->BlockLength  = 0;
                voice->position     = 0;
                return MV_GetNextVOCBlock(voice);
            }
            break;

        case 1 :
            // Sound data block
            voice->bits  = 8;
            voice->channels = voicemode + 1;
            if (lastblocktype != 8)
            {
                tc = (uint32_t)*ptr << 8;
                packtype = *(ptr + 1);
            }

            ptr += 2;
            blocklength -= 2;

            samplespeed = 256000000L / (voice->channels * (65536 - tc));

            // Skip packed or stereo data
            if ((packtype != 0) || (voicemode != 0 && voicemode != 1))
                ptr += blocklength;
            else
                done = TRUE;

            if ((uint32_t)(ptr - (uint8_t *)voice->rawdataptr) >= voice->rawdatasiz)
                goto end_of_data;

            voicemode = 0;
            break;

        case 2 :
            // Sound continuation block
            samplespeed = voice->SamplingRate;
            done = TRUE;
            break;

        case 3 :
            // Silence
        case 4 :
            // Marker
        case 5 :
            // ASCII string
            // All not implemented.
            ptr += blocklength;
            break;

        case 6 :
            // Repeat begin
            if (voice->Loop.End == nullptr)
            {
                voice->Loop.Count = B_LITTLE16(*(uint16_t const *)ptr);
                voice->Loop.Start = (char *)((intptr_t) ptr + blocklength);
            }
            ptr += blocklength;
            break;

        case 7 :
            // Repeat end
            ptr += blocklength;
            if (lastblocktype == 6)
                voice->Loop.Count = 0;
            else
            {
                if ((voice->Loop.Count > 0) && (voice->Loop.Start != nullptr))
                {
                    ptr = (uint8_t const *) voice->Loop.Start;

                    if (voice->Loop.Count < 0xffff)
                    {
                        if (--voice->Loop.Count == 0)
                            voice->Loop.Start = nullptr;
                    }
                }
            }
            break;

        case 8 :
            // Extended block
            voice->bits  = 8;
            voice->channels = 1;
            tc = B_LITTLE16(*(uint16_t const *)ptr);
            packtype = *(ptr + 2);
            voicemode = *(ptr + 3);
            ptr += blocklength;
            break;

        case 9 :
            // New sound data block
            samplespeed = B_LITTLE32(*(uint32_t const *)ptr);
            BitsPerSample = (unsigned)*(ptr + 4);
            Channels = (unsigned)*(ptr + 5);
            Format = (unsigned)B_LITTLE16(*(uint16_t const *)(ptr + 6));

            if ((BitsPerSample == 8) && (Channels == 1 || Channels == 2) && (Format == VOC_8BIT))
            {
                ptr         += 12;
                blocklength -= 12;
                voice->bits  = 8;
                voice->channels = Channels;
                done         = TRUE;
            }
            else if ((BitsPerSample == 16) && (Channels == 1 || Channels == 2) && (Format == VOC_16BIT))
            {
                ptr         += 12;
                blocklength -= 12;
                voice->bits  = 16;
                voice->channels = Channels;
                done         = TRUE;
            }
            else
            {
                ptr += blocklength;
            }

            // CAUTION:
            //  SNAKRM.VOC is corrupt!  blocklength gets us beyond the
            //  end of the file.
            if ((uint32_t)(ptr - (uint8_t *)voice->rawdataptr) >= voice->rawdatasiz)
                goto end_of_data;

            break;

        default :
            // Unknown data.  Probably not a VOC file.
            done = 2;
            break;
        }

        lastblocktype = blocktype;
    }
    while (!done);

    if (done != 2)
    {
        voice->NextBlock    = (char const *)ptr + blocklength;
        voice->sound        = (char const *)ptr;

        // CODEDUP multivoc.c MV_SetVoicePitch
        voice->SamplingRate = samplespeed;
        voice->RateScale    = divideu64((uint64_t)voice->SamplingRate * voice->PitchScale, MV_MixRate);

        // Multiply by MV_MIXBUFFERSIZE - 1
        voice->FixedPointBufferSize = (voice->RateScale * MV_MIXBUFFERSIZE) -
                                      voice->RateScale;

        if (voice->Loop.End != nullptr)
        {
            if (blocklength > (uintptr_t)voice->Loop.End)
                blocklength = (uintptr_t)voice->Loop.End;
            else
                voice->Loop.End = (char *)blocklength;

            voice->Loop.Start = voice->sound + (uintptr_t)voice->Loop.Start;
            voice->Loop.End   = voice->sound + (uintptr_t)voice->Loop.End;
            voice->Loop.Size  = voice->Loop.End - voice->Loop.Start;
        }

        if (voice->bits == 16)
            blocklength /= 2;

        if (voice->channels == 2)
            blocklength /= 2;

        voice->position     = 0;
        voice->length       = min<uint32_t>(blocklength, 0x8000u);
        voice->BlockLength  = blocklength - voice->length;
        voice->length     <<= 16;

        MV_SetVoiceMixMode(voice);

        return KeepPlaying;
    }

    return NoMoreData;
}

static playbackstatus MV_GetNextRAWBlock(VoiceNode *voice)
{
    if (voice->BlockLength == 0)
    {
        if (voice->Loop.Start == NULL)
            return NoMoreData;

        voice->BlockLength = voice->Loop.Size;
        voice->NextBlock   = voice->Loop.Start;
        voice->length      = 0;
        voice->position    = 0;
    }

    voice->sound        = voice->NextBlock;
    voice->position    -= voice->length;
    voice->length       = min(voice->BlockLength, 0x8000u);
    voice->NextBlock   += voice->length * (voice->channels * voice->bits / 8);
    voice->BlockLength -= voice->length;
    voice->length     <<= 16;

    return KeepPlaying;
}

int MV_PlayWAV3D(char *ptr, uint32_t length, int loophow, int pitchoffset, int angle, int distance,
                     int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_Error;

    if (distance < 0)
    {
        distance  = -distance;
        angle    += MV_NUMPANPOSITIONS / 2;
    }

    int const vol = MIX_VOLUME(distance);

    // Ensure angle is within 0 - 127
    angle &= MV_MAXPANPOSITION;

    return MV_PlayWAV(ptr, length, loophow, -1, pitchoffset, max(0, 255 - distance),
        MV_PanTable[angle][vol].left, MV_PanTable[angle][vol].right, priority, volume, callbackval);
}

int MV_PlayWAV(char *ptr, uint32_t length, int loopstart, int loopend, int pitchoffset, int vol,
                   int left, int right, int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_Error;

    riff_header   riff;
    memcpy(&riff, ptr, sizeof(riff_header));
    riff.file_size   = B_LITTLE32(riff.file_size);
    riff.format_size = B_LITTLE32(riff.format_size);

    if ((memcmp(riff.RIFF, "RIFF", 4) != 0) || (memcmp(riff.WAVE, "WAVE", 4) != 0) || (memcmp(riff.fmt, "fmt ", 4) != 0))
        return MV_SetErrorCode(MV_InvalidFile);

    format_header format;
    memcpy(&format, ptr + sizeof(riff_header), sizeof(format_header));
    format.wFormatTag      = B_LITTLE16(format.wFormatTag);
    format.nChannels       = B_LITTLE16(format.nChannels);
    format.nSamplesPerSec  = B_LITTLE32(format.nSamplesPerSec);
    format.nAvgBytesPerSec = B_LITTLE32(format.nAvgBytesPerSec);
    format.nBlockAlign     = B_LITTLE16(format.nBlockAlign);
    format.nBitsPerSample  = B_LITTLE16(format.nBitsPerSample);

    data_header   data;
    memcpy(&data, ptr + sizeof(riff_header) + riff.format_size, sizeof(data_header));
    data.size = B_LITTLE32(data.size);

    // Check if it's PCM data.
    if (format.wFormatTag != 1 || (format.nChannels != 1 && format.nChannels != 2) ||
        ((format.nBitsPerSample != 8) && (format.nBitsPerSample != 16)) || memcmp(data.DATA, "data", 4) != 0)
        return MV_SetErrorCode(MV_InvalidFile);

    // Request a voice from the voice pool

    auto voice = MV_AllocVoice(priority);

    if (voice == nullptr)
        return MV_SetErrorCode(MV_NoVoices);

    voice->wavetype    = FMT_WAV;
    voice->bits        = format.nBitsPerSample;
    voice->channels    = format.nChannels;
    voice->GetSound    = MV_GetNextWAVBlock;

    int blocklen = data.size;

    if (voice->bits == 16)
    {
        data.size  &= ~1;
        blocklen     /= 2;
    }

    if (voice->channels == 2)
    {
        data.size &= ~1;
        blocklen    /= 2;
    }

    voice->rawdataptr   = (uint8_t *)ptr;
    voice->rawdatasiz   = length;
    voice->position     = 0;
    voice->BlockLength  = blocklen;
    voice->NextBlock    = (char *)((intptr_t)ptr + sizeof(riff_header) + riff.format_size + sizeof(data_header));
    voice->priority     = priority;
    voice->callbackval  = callbackval;
    voice->Loop.Start   = loopstart >= 0 ? voice->NextBlock : nullptr;
    voice->Loop.End     = nullptr;
    voice->Loop.Count   = 0;
    voice->Loop.Size    = loopend > 0 ? loopend - loopstart + 1 : blocklen;

    MV_SetVoicePitch(voice, format.nSamplesPerSec, pitchoffset);
    MV_SetVoiceVolume(voice, vol, left, right, volume);
    MV_PlayVoice(voice);

    return voice->handle;
}

int MV_PlayVOC3D(char *ptr, uint32_t length, int loophow, int pitchoffset, int angle,
                     int distance, int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_Error;

    if (distance < 0)
    {
        distance  = -distance;
        angle    += MV_NUMPANPOSITIONS / 2;
    }

    int const vol = MIX_VOLUME(distance);

    // Ensure angle is within 0 - 127
    angle &= MV_MAXPANPOSITION;

    return MV_PlayVOC(ptr, length, loophow, -1, pitchoffset, max(0, 255 - distance),
        MV_PanTable[angle][vol].left, MV_PanTable[angle][vol].right, priority, volume, callbackval);
}

int MV_PlayVOC(char *ptr, uint32_t length, int loopstart, int loopend, int pitchoffset, int vol,
                   int left, int right, int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_Error;

    // Make sure it looks like a valid VOC file.
    if (memcmp(ptr, "Creative Voice File", 19) != 0)
        return MV_SetErrorCode(MV_InvalidFile);

    // Request a voice from the voice pool
    auto voice = MV_AllocVoice(priority);

    if (voice == nullptr)
        return MV_SetErrorCode(MV_NoVoices);

    voice->rawdataptr  = (uint8_t *)ptr;
    voice->rawdatasiz  = length;
    voice->wavetype    = FMT_VOC;
    voice->bits        = 8;
    voice->channels    = 1;
    voice->GetSound    = MV_GetNextVOCBlock;
    voice->NextBlock   = ptr + B_LITTLE16(*(uint16_t *)(ptr + 0x14));
    voice->PitchScale  = PITCH_GetScale(pitchoffset);
    voice->priority    = priority;
    voice->callbackval = callbackval;
    voice->Loop        = { loopstart >= 0 ? voice->NextBlock : nullptr, nullptr, 0, (uint32_t)(loopend - loopstart + 1) };

    MV_SetVoiceVolume(voice, vol, left, right, volume);
    MV_PlayVoice(voice);

    return voice->handle;
}

int MV_PlayRAW(char *ptr, uint32_t length, int rate, char *loopstart, char *loopend, int pitchoffset, int vol,
                   int left, int right, int priority, fix16_t volume, intptr_t callbackval)
{
    if (!MV_Installed)
        return MV_Error;

    // Request a voice from the voice pool
    auto voice = MV_AllocVoice(priority);

    if (voice == nullptr)
        return MV_SetErrorCode(MV_NoVoices);

    voice->rawdataptr  = (uint8_t *)ptr;
    voice->rawdatasiz  = length;
    voice->wavetype    = FMT_RAW;
    voice->bits        = 8;
    voice->channels    = 1;
    voice->GetSound    = MV_GetNextRAWBlock;
    voice->NextBlock   = ptr;
    voice->position    = 0;
    voice->BlockLength = length;
    voice->PitchScale  = PITCH_GetScale(pitchoffset);
    voice->priority    = priority;
    voice->callbackval = callbackval;
    voice->Loop        = { loopstart, loopend, 0, (uint32_t)(loopend - loopstart + 1) };
    voice->volume      = volume;

    MV_SetVoicePitch(voice, rate, pitchoffset);
    MV_SetVoiceVolume(voice, vol, left, right, volume);
    MV_PlayVoice(voice);

    return voice->handle;
}
