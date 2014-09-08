/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <errno.h>

#include "alMain.h"
#include "alu.h"
#include "threads.h"
#include "compat.h"

#include "backends/base.h"


static const ALCchar waveDevice[] = "Wave File Writer";

static const ALubyte SUBTYPE_PCM[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};
static const ALubyte SUBTYPE_FLOAT[] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa,
    0x00, 0x38, 0x9b, 0x71
};

static const ALuint channel_masks[] = {
    0, /* invalid */
    0x4, /* Mono */
    0x1 | 0x2, /* Stereo */
    0, /* 3 channel */
    0x1 | 0x2 | 0x10 | 0x20, /* Quad */
    0, /* 5 channel */
    0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20, /* 5.1 */
    0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400, /* 6.1 */
    0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400, /* 7.1 */
};


static void fwrite16le(ALushort val, FILE *f)
{
    fputc(val&0xff, f);
    fputc((val>>8)&0xff, f);
}

static void fwrite32le(ALuint val, FILE *f)
{
    fputc(val&0xff, f);
    fputc((val>>8)&0xff, f);
    fputc((val>>16)&0xff, f);
    fputc((val>>24)&0xff, f);
}


typedef struct ALCwaveBackend {
    DERIVE_FROM_TYPE(ALCbackend);

    FILE *mFile;
    long mDataStart;

    ALvoid *mBuffer;
    ALuint mSize;

    volatile int killNow;
    althrd_t thread;
} ALCwaveBackend;

static int ALCwaveBackend_mixerProc(void *ptr);

static void ALCwaveBackend_Construct(ALCwaveBackend *self, ALCdevice *device);
static DECLARE_FORWARD(ALCwaveBackend, ALCbackend, void, Destruct)
static ALCenum ALCwaveBackend_open(ALCwaveBackend *self, const ALCchar *name);
static void ALCwaveBackend_close(ALCwaveBackend *self);
static ALCboolean ALCwaveBackend_reset(ALCwaveBackend *self);
static ALCboolean ALCwaveBackend_start(ALCwaveBackend *self);
static void ALCwaveBackend_stop(ALCwaveBackend *self);
static DECLARE_FORWARD2(ALCwaveBackend, ALCbackend, ALCenum, captureSamples, void*, ALCuint)
static DECLARE_FORWARD(ALCwaveBackend, ALCbackend, ALCuint, availableSamples)
static DECLARE_FORWARD(ALCwaveBackend, ALCbackend, ALint64, getLatency)
static DECLARE_FORWARD(ALCwaveBackend, ALCbackend, void, lock)
static DECLARE_FORWARD(ALCwaveBackend, ALCbackend, void, unlock)
DECLARE_DEFAULT_ALLOCATORS(ALCwaveBackend)

DEFINE_ALCBACKEND_VTABLE(ALCwaveBackend);


static void ALCwaveBackend_Construct(ALCwaveBackend *self, ALCdevice *device)
{
    ALCbackend_Construct(STATIC_CAST(ALCbackend, self), device);
    SET_VTABLE2(ALCwaveBackend, ALCbackend, self);

    self->mFile = NULL;
    self->mDataStart = -1;

    self->mBuffer = NULL;
    self->mSize = 0;

    self->killNow = 1;
}


static int ALCwaveBackend_mixerProc(void *ptr)
{
    ALCwaveBackend *self = (ALCwaveBackend*)ptr;
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    struct timespec now, start;
    ALint64 avail, done;
    ALuint frameSize;
    size_t fs;
    const long restTime = (long)((ALuint64)device->UpdateSize * 1000000000 /
                                 device->Frequency / 2);

    althrd_setname(althrd_current(), MIXER_THREAD_NAME);

    frameSize = FrameSizeFromDevFmt(device->FmtChans, device->FmtType);

    done = 0;
    if(altimespec_get(&start, AL_TIME_UTC) != AL_TIME_UTC)
    {
        ERR("Failed to get starting time\n");
        return 1;
    }
    while(!self->killNow && device->Connected)
    {
        if(altimespec_get(&now, AL_TIME_UTC) != AL_TIME_UTC)
        {
            ERR("Failed to get current time\n");
            return 1;
        }

        avail  = (now.tv_sec - start.tv_sec) * device->Frequency;
        avail += (ALint64)(now.tv_nsec - start.tv_nsec) * device->Frequency / 1000000000;
        if(avail < done)
        {
            /* Oops, time skipped backwards. Reset the number of samples done
             * with one update available since we (likely) just came back from
             * sleeping. */
            done = avail - device->UpdateSize;
        }

        if(avail-done < device->UpdateSize)
            al_nssleep(restTime);
        else while(avail-done >= device->UpdateSize)
        {
            aluMixData(device, self->mBuffer, device->UpdateSize);
            done += device->UpdateSize;

            if(!IS_LITTLE_ENDIAN)
            {
                ALuint bytesize = BytesFromDevFmt(device->FmtType);
                ALubyte *bytes = self->mBuffer;
                ALuint i;

                if(bytesize == 1)
                {
                    for(i = 0;i < self->mSize;i++)
                        fputc(bytes[i], self->mFile);
                }
                else if(bytesize == 2)
                {
                    for(i = 0;i < self->mSize;i++)
                        fputc(bytes[i^1], self->mFile);
                }
                else if(bytesize == 4)
                {
                    for(i = 0;i < self->mSize;i++)
                        fputc(bytes[i^3], self->mFile);
                }
            }
            else
            {
                fs = fwrite(self->mBuffer, frameSize, device->UpdateSize,
                            self->mFile);
                (void)fs;
            }
            if(ferror(self->mFile))
            {
                ERR("Error writing to file\n");
                ALCdevice_Lock(device);
                aluHandleDisconnect(device);
                ALCdevice_Unlock(device);
                break;
            }
        }
    }

    return 0;
}


static ALCenum ALCwaveBackend_open(ALCwaveBackend *self, const ALCchar *name)
{
    ALCdevice *device;
    const char *fname;

    fname = GetConfigValue("wave", "file", "");
    if(!fname[0]) return ALC_INVALID_VALUE;

    if(!name)
        name = waveDevice;
    else if(strcmp(name, waveDevice) != 0)
        return ALC_INVALID_VALUE;

    self->mFile = al_fopen(fname, "wb");
    if(!self->mFile)
    {
        ERR("Could not open file '%s': %s\n", fname, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    device = STATIC_CAST(ALCbackend, self)->mDevice;
    al_string_copy_cstr(&device->DeviceName, name);

    return ALC_NO_ERROR;
}

static void ALCwaveBackend_close(ALCwaveBackend *self)
{
    if(self->mFile)
        fclose(self->mFile);
    self->mFile = NULL;
}

static ALCboolean ALCwaveBackend_reset(ALCwaveBackend *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;
    ALuint channels=0, bits=0;
    size_t val;

    fseek(self->mFile, 0, SEEK_SET);
    clearerr(self->mFile);

    switch(device->FmtType)
    {
        case DevFmtByte:
            device->FmtType = DevFmtUByte;
            break;
        case DevFmtUShort:
            device->FmtType = DevFmtShort;
            break;
        case DevFmtUInt:
            device->FmtType = DevFmtInt;
            break;
        case DevFmtUByte:
        case DevFmtShort:
        case DevFmtInt:
        case DevFmtFloat:
            break;
    }
    bits = BytesFromDevFmt(device->FmtType) * 8;
    channels = ChannelsFromDevFmt(device->FmtChans);

    fprintf(self->mFile, "RIFF");
    fwrite32le(0xFFFFFFFF, self->mFile); // 'RIFF' header len; filled in at close

    fprintf(self->mFile, "WAVE");

    fprintf(self->mFile, "fmt ");
    fwrite32le(40, self->mFile); // 'fmt ' header len; 40 bytes for EXTENSIBLE

    // 16-bit val, format type id (extensible: 0xFFFE)
    fwrite16le(0xFFFE, self->mFile);
    // 16-bit val, channel count
    fwrite16le(channels, self->mFile);
    // 32-bit val, frequency
    fwrite32le(device->Frequency, self->mFile);
    // 32-bit val, bytes per second
    fwrite32le(device->Frequency * channels * bits / 8, self->mFile);
    // 16-bit val, frame size
    fwrite16le(channels * bits / 8, self->mFile);
    // 16-bit val, bits per sample
    fwrite16le(bits, self->mFile);
    // 16-bit val, extra byte count
    fwrite16le(22, self->mFile);
    // 16-bit val, valid bits per sample
    fwrite16le(bits, self->mFile);
    // 32-bit val, channel mask
    fwrite32le(channel_masks[channels], self->mFile);
    // 16 byte GUID, sub-type format
    val = fwrite(((bits==32) ? SUBTYPE_FLOAT : SUBTYPE_PCM), 1, 16, self->mFile);
    (void)val;

    fprintf(self->mFile, "data");
    fwrite32le(0xFFFFFFFF, self->mFile); // 'data' header len; filled in at close

    if(ferror(self->mFile))
    {
        ERR("Error writing header: %s\n", strerror(errno));
        return ALC_FALSE;
    }
    self->mDataStart = ftell(self->mFile);

    SetDefaultWFXChannelOrder(device);

    return ALC_TRUE;
}

static ALCboolean ALCwaveBackend_start(ALCwaveBackend *self)
{
    ALCdevice *device = STATIC_CAST(ALCbackend, self)->mDevice;

    self->mSize = device->UpdateSize * FrameSizeFromDevFmt(device->FmtChans, device->FmtType);
    self->mBuffer = malloc(self->mSize);
    if(!self->mBuffer)
    {
        ERR("Buffer malloc failed\n");
        return ALC_FALSE;
    }

    self->killNow = 0;
    if(althrd_create(&self->thread, ALCwaveBackend_mixerProc, self) != althrd_success)
    {
        free(self->mBuffer);
        self->mBuffer = NULL;
        self->mSize = 0;
        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static void ALCwaveBackend_stop(ALCwaveBackend *self)
{
    ALuint dataLen;
    long size;
    int res;

    if(self->killNow)
        return;

    self->killNow = 1;
    althrd_join(self->thread, &res);

    free(self->mBuffer);
    self->mBuffer = NULL;

    size = ftell(self->mFile);
    if(size > 0)
    {
        dataLen = size - self->mDataStart;
        if(fseek(self->mFile, self->mDataStart-4, SEEK_SET) == 0)
            fwrite32le(dataLen, self->mFile); // 'data' header len
        if(fseek(self->mFile, 4, SEEK_SET) == 0)
            fwrite32le(size-8, self->mFile); // 'WAVE' header len
    }
}


typedef struct ALCwaveBackendFactory {
    DERIVE_FROM_TYPE(ALCbackendFactory);
} ALCwaveBackendFactory;
#define ALCWAVEBACKENDFACTORY_INITIALIZER { { GET_VTABLE2(ALCwaveBackendFactory, ALCbackendFactory) } }

ALCbackendFactory *ALCwaveBackendFactory_getFactory(void);

static ALCboolean ALCwaveBackendFactory_init(ALCwaveBackendFactory *self);
static DECLARE_FORWARD(ALCwaveBackendFactory, ALCbackendFactory, void, deinit)
static ALCboolean ALCwaveBackendFactory_querySupport(ALCwaveBackendFactory *self, ALCbackend_Type type);
static void ALCwaveBackendFactory_probe(ALCwaveBackendFactory *self, enum DevProbe type);
static ALCbackend* ALCwaveBackendFactory_createBackend(ALCwaveBackendFactory *self, ALCdevice *device, ALCbackend_Type type);
DEFINE_ALCBACKENDFACTORY_VTABLE(ALCwaveBackendFactory);


ALCbackendFactory *ALCwaveBackendFactory_getFactory(void)
{
    static ALCwaveBackendFactory factory = ALCWAVEBACKENDFACTORY_INITIALIZER;
    return STATIC_CAST(ALCbackendFactory, &factory);
}


static ALCboolean ALCwaveBackendFactory_init(ALCwaveBackendFactory* UNUSED(self))
{
    return ALC_TRUE;
}

static ALCboolean ALCwaveBackendFactory_querySupport(ALCwaveBackendFactory* UNUSED(self), ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
        return !!ConfigValueExists("wave", "file");
    return ALC_FALSE;
}

static void ALCwaveBackendFactory_probe(ALCwaveBackendFactory* UNUSED(self), enum DevProbe type)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
            AppendAllDevicesList(waveDevice);
            break;
        case CAPTURE_DEVICE_PROBE:
            break;
    }
}

static ALCbackend* ALCwaveBackendFactory_createBackend(ALCwaveBackendFactory* UNUSED(self), ALCdevice *device, ALCbackend_Type type)
{
    if(type == ALCbackend_Playback)
    {
        ALCwaveBackend *backend;

        backend = ALCwaveBackend_New(sizeof(*backend));
        if(!backend) return NULL;
        memset(backend, 0, sizeof(*backend));

        ALCwaveBackend_Construct(backend, device);

        return STATIC_CAST(ALCbackend, backend);
    }

    return NULL;
}

void alc_wave_probe(enum DevProbe type)
{
    if(!ConfigValueExists("wave", "file"))
        return;

    switch(type)
    {
        case ALL_DEVICE_PROBE:
            AppendAllDevicesList(waveDevice);
            break;
        case CAPTURE_DEVICE_PROBE:
            break;
    }
}
