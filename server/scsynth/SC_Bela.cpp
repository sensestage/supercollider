/*
    Bela audio driver for SuperCollider.
    Copyright (c) 2016 Dan Stowell. All rights reserved.
    Copyright (c) 2016 Marije Baalman. All rights reserved.
    Copyright (c) 2016 Giulio Moro. All rights reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA

    This file contains elements from SC_PortAudio.cpp and SC_Jack.cpp,
    copyright their authors, and published under the same licence.
*/
#include "SC_CoreAudio.h"
#include <stdarg.h>
#include "SC_Prototypes.h"
#include "SC_HiddenWorld.h"
#include "SC_WorldOptions.h"
#include "SC_Time.hpp"
#include "SC_BelaScope.h"
#include <math.h>
#include <stdlib.h>
#include <cobalt/time.h> // needed for CLOCK_HOST_REALTIME
#include <cobalt/stdio.h> // rt_vprintf

extern "C" {
// This will be wrapped by Xenomai without requiring linker flags
int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp);
}

#include "Bela.h"
// Xenomai-specific includes
#include <sys/mman.h>

#if (BELA_MAJOR_VERSION == 1 && BELA_MINOR_VERSION < 9)
#    error You need at least Bela API 1.9
#endif

using namespace std;

// Audio driver API implementation
int32 server_timeseed() { return timeSeed(); }
int64 gOSCoffset = 0;
int64 oscTimeNow() { return OSCTime(getTime()); }
void initializeScheduler() { gOSCoffset = oscTimeNow(); }

class SC_BelaDriver final : public SC_AudioDriver {
public:
    SC_BelaDriver(World* inWorld);
    virtual ~SC_BelaDriver();

    void BelaAudioCallback(BelaContext* belaContext);
    bool BelaSetup(BelaContext* belaContext);
    void SignalReceived(int signal);

    static SC_BelaDriver* s_instance;
    static SC_BelaDriver* Construct(World* inWorld) {
        if (s_instance != nullptr) {
            scprintf("*** ERROR: Asked to construct a second instance of SC_BelaDriver.\n");
            std::exit(1);
        }

        s_instance = new SC_BelaDriver(inWorld);
        return s_instance;
    }

protected:
    // Driver interface methods
    bool DriverSetup(int* outNumSamplesPerCallback, double* outSampleRate) override;
    bool DriverStart() override;
    bool DriverStop() override;

private:
    int mInputChannelCount, mOutputChannelCount;
    uint32 mSCBufLength;
};

SC_BelaDriver* SC_BelaDriver::s_instance = nullptr;

SC_AudioDriver* SC_NewAudioDriver(World* inWorld) { return SC_BelaDriver::Construct(inWorld); }

SC_BelaDriver::SC_BelaDriver(World* inWorld): SC_AudioDriver(inWorld), mSCBufLength(inWorld->mBufLength) {
    mStartHostSecs = 0;
}

SC_BelaDriver::~SC_BelaDriver() {
    // Clean up any resources allocated for audio
    Bela_cleanupAudio();
    scprintf("SC_BelaDriver: >>Bela_cleanupAudio\n");
    s_instance = nullptr;
    if (mWorld->mBelaScope)
        delete mWorld->mBelaScope;
}

static float gBelaSampleRate;

// Return true on success; returning false halts the program.
bool SC_BelaDriver::BelaSetup(BelaContext* belaContext) {
    gBelaSampleRate = belaContext->audioSampleRate;
    if (mWorld->mBelaMaxScopeChannels > 0)
        mWorld->mBelaScope = new BelaScope(mWorld->mBelaMaxScopeChannels, gBelaSampleRate, belaContext->audioFrames);
    return true;
}

bool sc_belaSetup(BelaContext* belaContext, void* userData) {
    // cast void pointer
    SC_BelaDriver* belaDriver = (SC_BelaDriver*)userData;
    return belaDriver->BelaSetup(belaContext);
}

void sc_belaRender(BelaContext* belaContext, void* userData) {
    SC_BelaDriver* driver = (SC_BelaDriver*)userData;

    driver->BelaAudioCallback(belaContext);
}

void sc_belaAudioThreadDone(BelaContext*, void* userData) {
    SC_BelaDriver* driver = (SC_BelaDriver*)userData;
    if (driver)
        driver->SignalReceived(0);
}

void sc_belaSignal(int arg) {
    if (SC_BelaDriver::s_instance != nullptr)
        SC_BelaDriver::s_instance->SignalReceived(arg);
}

void sc_SetDenormalFlags();

void SC_BelaDriver::BelaAudioCallback(BelaContext* belaContext) {
    struct timespec tspec;

    sc_SetDenormalFlags();
    World* world = mWorld;
    // add a pointer to belaWorld
    world->mBelaContext = belaContext;

    // NOTE: code here is adapted from the SC_Jack.cpp, the version not using the DLL

    // Use Xenomai-friendly clock_gettime()
    __wrap_clock_gettime(CLOCK_HOST_REALTIME, &tspec);

    double hostSecs = (double)tspec.tv_sec + (double)tspec.tv_nsec * 1.0e-9;
    double sampleTime = static_cast<double>(belaContext->audioFramesElapsed);

    if (mStartHostSecs == 0) {
        mStartHostSecs = hostSecs;
        mStartSampleTime = sampleTime;
    } else {
        double instSampleRate = (sampleTime - mPrevSampleTime) / (hostSecs - mPrevHostSecs);
        double smoothSampleRate = mSmoothSampleRate;
        smoothSampleRate = smoothSampleRate + 0.002 * (instSampleRate - smoothSampleRate);
        if (fabs(smoothSampleRate - mSampleRate) > 10.) {
            smoothSampleRate = mSampleRate;
        }
        mOSCincrement = (int64)(mOSCincrementNumerator / smoothSampleRate);
        mSmoothSampleRate = smoothSampleRate;
    }

    mPrevHostSecs = hostSecs;
    mPrevSampleTime = sampleTime;

    try {
        mFromEngine.Free();
        mToEngine.Perform();
        mOscPacketsToEngine.Perform();

        const uint32_t numInputs = belaContext->audioInChannels;
        const uint32_t numOutputs = belaContext->audioOutChannels;

        int numSamples = NumSamplesPerCallback();
        int bufFrames = mWorld->mBufLength;
        int numBufs = numSamples / bufFrames;

        float* inBuses = mWorld->mAudioBus + mWorld->mNumOutputs * bufFrames;
        float* outBuses = mWorld->mAudioBus;
        int32* inTouched = mWorld->mAudioBusTouched + mWorld->mNumOutputs;
        int32* outTouched = mWorld->mAudioBusTouched;

        int minInputs = sc_min(numInputs, mWorld->mNumInputs);
        int minOutputs = sc_min(numOutputs, mWorld->mNumOutputs);

        int anaInputs = 0;
        if (numInputs < (int)mWorld->mNumInputs) {
            anaInputs = sc_min(belaContext->analogInChannels, (int)(mWorld->mNumInputs - numInputs));
        }
        int anaOutputs = 0;
        if (numOutputs < (int)mWorld->mNumOutputs) {
            anaOutputs = sc_min(belaContext->analogOutChannels, (int)(mWorld->mNumOutputs - numOutputs));
        }

        int bufFramePos = 0;

        // THIS IS TO DO LATER -- LOOK AT CACHEING AND CONSTING TO IMPROVE EFFICIENCY
        // cache I/O buffers
        // for (int i = 0; i < minInputs; ++i) {
        //	inBuffers[i] = (sc_jack_sample_t*)jack_port_get_buffer(inPorts[i], numSamples);
        //}
        //
        // for (int i = 0; i < minOutputs; ++i) {
        //	outBuffers[i] = (sc_jack_sample_t*)jack_port_get_buffer(outPorts[i], numSamples);
        //}

        // main loop
        int64 oscTime = mOSCbuftime =
            ((int64)(tspec.tv_sec + kSECONDS_FROM_1900_to_1970) << 32) + (int64)(tspec.tv_nsec * kNanosToOSCunits);

        int64 oscInc = mOSCincrement;
        double oscToSamples = mOSCtoSamples;

        // clear out anything left over in audioOut buffer
        for (int i = 0; i < belaContext->audioFrames * belaContext->audioOutChannels; i++) {
            belaContext->audioOut[i] = 0;
        }

        for (int i = 0; i < numBufs; ++i, mWorld->mBufCounter++, bufFramePos += bufFrames) {
            int32 bufCounter = mWorld->mBufCounter;
            int32* tch;

            // copy+touch inputs
            tch = inTouched;
            memcpy(inBuses, belaContext->audioIn, sizeof(belaContext->audioIn[0]) * bufFrames * minInputs);
            for (int k = 0; k < minInputs; ++k) {
                *tch++ = bufCounter;
            }

            memcpy(inBuses + minInputs * bufFrames, belaContext->analogIn,
                   sizeof(belaContext->analogIn[0]) * bufFrames * anaInputs);
            for (int k = minInputs; k < (minInputs + anaInputs); ++k) {
                *tch++ = bufCounter;
            }

            // run engine
            int64 schedTime;
            int64 nextTime = oscTime + oscInc;

            while ((schedTime = mScheduler.NextTime()) <= nextTime) {
                float diffTime = (float)(schedTime - oscTime) * oscToSamples + 0.5;
                float diffTimeFloor = floor(diffTime);
                world->mSampleOffset = (int)diffTimeFloor;
                world->mSubsampleOffset = diffTime - diffTimeFloor;

                if (world->mSampleOffset < 0)
                    world->mSampleOffset = 0;
                else if (world->mSampleOffset >= world->mBufLength)
                    world->mSampleOffset = world->mBufLength - 1;

                SC_ScheduledEvent event = mScheduler.Remove();
                event.Perform();
            }

            world->mSampleOffset = 0;
            world->mSubsampleOffset = 0.f;
            World_Run(world);

            // copy touched outputs
            tch = outTouched;

            for (int k = 0; k < minOutputs; ++k) {
                if (*tch++ == bufCounter) {
                    memcpy(belaContext->audioOut + k * bufFrames, outBuses + k * bufFrames,
                           sizeof(belaContext->audioOut[0]) * bufFrames);
                }
            }

            for (int k = minOutputs; k < (minOutputs + anaOutputs); ++k) {
                if (*tch++ == bufCounter) {
                    unsigned int analogChannel = k - minOutputs; // starting at 0
                    memcpy(belaContext->analogOut + analogChannel * bufFrames, outBuses + k * bufFrames,
                           sizeof(belaContext->analogOut[0]) * bufFrames);
                }
            }

            // advance OSC time
            mOSCbuftime = oscTime = nextTime;
        }

        if (mWorld->mBelaScope)
            mWorld->mBelaScope->logBuffer();

    } catch (std::exception& exc) {
        scprintf("SC_BelaDriver: exception in real time: %s\n", exc.what());
    } catch (...) {
        scprintf("SC_BelaDriver: unknown exception in real time\n");
    }

    mAudioSync.Signal();
}

// ====================================================================

bool SC_BelaDriver::DriverSetup(int* outNumSamples, double* outSampleRate) {
    BelaInitSettings* settings = Bela_InitSettings_alloc();
    Bela_defaultSettings(settings);
    settings->setup = sc_belaSetup;
    settings->render = sc_belaRender;
    // if the feature is supported on Bela, add a callback to be called when
    // the audio thread stops. This is useful e.g.: to gracefully exit from
    // scsynth when pressing the Bela button
    settings->audioThreadDone = sc_belaAudioThreadDone;
    settings->interleave = 0;
    settings->uniformSampleRate = 1;
    settings->analogOutputsPersist = 0;

    if (mPreferredHardwareBufferFrameSize) {
        settings->periodSize = mPreferredHardwareBufferFrameSize;
    }
    if (settings->periodSize != mSCBufLength) {
        scprintf("Warning in SC_BelaDriver::DriverSetup(): hardware buffer size (%i) different from SC audio buffer "
                 "size (%i). Changed the hardware buffer size to be equal to the SC audio buffer size .\n",
                 settings->periodSize, mSCBufLength);
        settings->periodSize = mSCBufLength;
    }
    // note that Bela doesn't give us an option to choose samplerate, since
    // it's baked-in for a given board, however this can be retrieved in sc_belaSetup()

    // configure the number of analog channels - this will determine their internal samplerate
    settings->useAnalog = 0;

    // explicitly requested number of analog channels
    int numAnalogIn = mWorld->mBelaAnalogInputChannels;
    int numAnalogOut = mWorld->mBelaAnalogOutputChannels;

    // Here is the deal. We need to know:
    // - how many real audio channels are available
    // - how many audio channels the user wants
    // - how many analog channels are available
    // before we can request Bela for:
    // - a given number of analog channels
    // - applying the audio expander capelet on these channels
    // Currently (as of 1.4.0) the Bela API does not allow to
    // know the number of audio channels available.

    BelaHwConfig* cfg = Bela_HwConfig_new(Bela_detectHw());
    int extraAudioIn = mWorld->mNumInputs - cfg->audioInChannels;
    int extraAudioOut = mWorld->mNumOutputs - cfg->audioOutChannels;
    // if we need more audio channels than there actually are audio
    // channels, make sure we have some extra analogs
    if (extraAudioIn > 0) {
        numAnalogIn = sc_max(numAnalogIn, extraAudioIn);
    }
    if (extraAudioOut > 0) {
        numAnalogOut = sc_max(numAnalogOut, extraAudioOut);
    }

    // snap the number of requested analog channels to the 0, 4, 8.
    // 4 will give same actual sample rate as audio, 8 will give half of it.
    if (numAnalogIn > 0) {
        if (numAnalogIn < 5) {
            numAnalogIn = 4;
        } else {
            numAnalogIn = 8;
        }
    }

    if (numAnalogOut > 0) {
        if (numAnalogOut < 5) {
            numAnalogOut = 4;
        } else {
            numAnalogOut = 8;
        }
    }

    // final check: right now the number of analog output channels on bela needs to be the same as analog input
    // channels. this is likely to change in the future, that is why we factored it out
    if (numAnalogOut != numAnalogIn) {
        // Chosing the maximum of the two
        numAnalogOut = sc_max(numAnalogOut, numAnalogIn);
        numAnalogIn = numAnalogOut;
        printf("Number of analog input channels must match number of analog outputs. Using %u for both\n", numAnalogIn);
    }
    settings->numAnalogInChannels = numAnalogIn;
    settings->numAnalogOutChannels = numAnalogOut;

    if (settings->numAnalogInChannels > 0 || settings->numAnalogOutChannels > 0) {
        settings->useAnalog = 1;
    }

    // enable the audio expander capelet for the first few "analog as audio" channels
    // inputs and ...
    for (int n = 0; n < extraAudioIn; ++n) {
        printf("Using analog in %d as audio in %d\n", n, n + cfg->audioInChannels);
        settings->audioExpanderInputs |= (1 << n);
    }

    // ... outputs
    for (int n = 0; n < extraAudioOut; ++n) {
        printf("Using analog out %d as audio out %d\n", n, n + cfg->audioOutChannels);
        settings->audioExpanderOutputs |= (1 << n);
    }

    // configure the number of digital channels
    settings->useDigital = 0;

    if (mWorld->mBelaDigitalChannels > 0) {
        settings->numDigitalChannels = mWorld->mBelaDigitalChannels;
        settings->useDigital = 1;
    }
    if ((mWorld->mBelaHeadphoneLevel >= -63.5)
        && (mWorld->mBelaHeadphoneLevel <= 0.)) { // headphone output level (0dB max; -63.5dB min)
        settings->headphoneLevel = mWorld->mBelaHeadphoneLevel;
    }
    if ((mWorld->mBelaPGAGainLeft >= 0) && (mWorld->mBelaPGAGainLeft <= 59.5)) { // (0db min; 59.5db max)
        settings->pgaGain[0] = mWorld->mBelaPGAGainLeft;
    }
    if ((mWorld->mBelaPGAGainRight >= 0) && (mWorld->mBelaPGAGainRight <= 59.5)) { // (0db min; 59.5db max)
        settings->pgaGain[1] = mWorld->mBelaPGAGainRight;
    }

    if (mWorld->mBelaSpeakerMuted) {
        settings->beginMuted = 1;
    } else {
        settings->beginMuted = 0;
    }
    if ((mWorld->mBelaDACLevel >= -63.5) && (mWorld->mBelaDACLevel <= 0.)) { // (0dB max; -63.5dB min)
        settings->dacLevel = mWorld->mBelaDACLevel;
    }
    if ((mWorld->mBelaADCLevel >= -12) && (mWorld->mBelaADCLevel <= 0.)) { // (0dB max; -12dB min)
        settings->adcLevel = mWorld->mBelaADCLevel;
    }

    settings->numMuxChannels = mWorld->mBelaNumMuxChannels;

    if ((mWorld->mBelaPRU == 0) || (mWorld->mBelaPRU == 1)) {
        settings->pruNumber = mWorld->mBelaPRU;
    }

    scprintf("SC_BelaDriver: >>DriverSetup - Running on PRU (%i)\nConfigured with \n (%i) analog input and (%i) analog "
             "output channels, (%i) digital channels, and (%i) multiplexer channels.\n HeadphoneLevel (%f dB), "
             "pga_gain_left (%f dB) and pga_gain_right (%f dB)\n DAC Level (%f dB), ADC Level (%f dB) "
             "oscilloscope channels (%i)\n",
             settings->pruNumber, settings->numAnalogInChannels, settings->numAnalogOutChannels,
             settings->numDigitalChannels, settings->numMuxChannels, settings->headphoneLevel, settings->pgaGain[0],
             settings->pgaGain[1], settings->dacLevel, settings->adcLevel, mWorld->mBelaMaxScopeChannels);
    if (settings->beginMuted == 1) {
        scprintf("Speakers are muted.\n");
    } else {
        scprintf("Speakers are not muted.\n");
    }

    settings->verbose = mWorld->mVerbosity;
    // This call will initialise the rendering system, which in the process
    // will result in a call to the user-defined setup() function.
    if (Bela_initAudio(settings, this) != 0) {
        scprintf("Error in SC_BelaDriver::DriverSetup(): unable to initialise audio\n");
        return false;
    }

    *outNumSamples = settings->periodSize;
    *outSampleRate = gBelaSampleRate;
    Bela_InitSettings_free(settings);
    Bela_HwConfig_delete(cfg);

    // Set up interrupt handler to catch Control-C and SIGTERM
    signal(SIGINT, sc_belaSignal);
    signal(SIGTERM, sc_belaSignal);

    return true;
}

bool SC_BelaDriver::DriverStart() {
    SetPrintFunc((PrintFunc)rt_vprintf); // Use Xenomai's realtime-friendly printing function
    if (Bela_startAudio()) {
        scprintf("Error in SC_BelaDriver::DriverStart(): unable to start real-time audio\n");
        return false;
    }
    return true;
}

bool SC_BelaDriver::DriverStop() {
    Bela_stopAudio();
    return true;
}

void SC_BelaDriver::SignalReceived(int signal) {
    scprintf("SC_BelaDriver: signal received: %d; terminating\n", signal);
    mWorld->hw->mTerminating = true;
    mWorld->hw->mQuitProgram->post();
}
