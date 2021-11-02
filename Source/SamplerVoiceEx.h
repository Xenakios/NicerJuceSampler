#pragma once

#include <JuceHeader.h>

/*
Nicer classes for implementing a sampler with Juce.
*/

class SamplerSoundEx : public juce::SynthesiserSound
{
public:
    SamplerSoundEx() {}
    // Convenience constructor to simply load an audio file
    SamplerSoundEx(juce::File audioFile, int rootNote, int lowNote, int highNote)
    : mRootNote(rootNote), mMidiRange(lowNote,highNote)
    {
        juce::AudioFormatManager man;
        man.registerBasicFormats();
        auto reader = man.createReaderFor(audioFile);
        if (reader)
        {
            mAudioData.setSize(reader->numChannels,reader->lengthInSamples);
            mSampleLength = reader->lengthInSamples;
            mSampleRate = reader->sampleRate;
            mChannels = reader->numChannels;
            reader->read(&mAudioData, 0, reader->lengthInSamples, 0, true, true);
            delete reader;
        }
    }
    // Take in an existing buffer, makes a copy of it!
    SamplerSoundEx(juce::AudioBuffer<float>& buf, double bufSampleRate, int rootNote, int lowNote, int highNote)
    : mSampleRate(bufSampleRate), mRootNote(rootNote), mMidiRange(lowNote,highNote)
    {
        mSampleLength = buf.getNumSamples();
        mChannels = buf.getNumChannels();
        mAudioData = buf;
    }
    bool appliesToNote (int midiNoteNumber) override
    {
        return mMidiRange.contains(midiNoteNumber);
    }
    bool appliesToChannel (int midiChannel) override
    {
        return true;
    }
    // if any of these return 0, the sample is not ready for playback
    int getSampleLength() const { return mSampleLength; }
    int getSampleNumChannels() const { return mChannels; }
    double getSampleRate() const { return mSampleRate; }
    
    int getRootNote() const { return mRootNote; }
    
    const juce::AudioBuffer<float>& getAudioBuffer() const
    {
        return mAudioData;
    }
private:
    int mSampleLength = 0;
    double mSampleRate = 0;
    int mChannels = 0;
    int mRootNote = 0;
    juce::Range<int> mMidiRange;
    juce::AudioBuffer<float> mAudioData;
    
};

// This class only implements resampling and looping the sample data from
// the SamplerSoundEx! So it is not a complete sampler voice by itself.
// Things like ADSR envelopes, filters and panning need to be implemented
// in subclasses. The demonstration class SamplerVoiceWithProcessing is provided.

class SamplerVoiceEx : public juce::SynthesiserVoice
{
public:
    SamplerVoiceEx()
    {
        mProcessBuffer.setSize(2,2048);
    }
    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<const SamplerSoundEx*> (sound) != nullptr;
    }
    
    void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound* s, int pitchWheel) override
    {
        if (auto* sound = dynamic_cast<const SamplerSoundEx*> (s))
        {
            mPitchRatio = std::pow (2.0, (midiNoteNumber - sound->getRootNote()) / 12.0)
            * sound->getSampleRate() / getSampleRate();
            
            mSourceSamplePosition = 0.0;
            mNoteVelocity = velocity;
            
            onStartNote(midiNoteNumber, velocity, pitchWheel);
        }
        else
        {
            jassertfalse; // this object can only play SamplerSoundExs!
        }
    }
    void stopNote (float velocity, bool allowTailOff) override
    {
        if (!allowTailOff)
        {
            clearCurrentNote();
        }
        onStopNote(velocity,allowTailOff);
    }
    
    void pitchWheelMoved (int newValue) override
    {
        
    }
    void controllerMoved (int controllerNumber, int newValue) override
    {
        
    }
    
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
    {
        if (auto* playingSound = static_cast<SamplerSoundEx*> (getCurrentlyPlayingSound().get()))
        {
            if (playingSound->getSampleNumChannels()==0)
                return;
            auto& data = playingSound->getAudioBuffer();
            const float* const inL = data.getReadPointer (0);
            const float* const inR = data.getNumChannels() > 1 ? data.getReadPointer (1) : nullptr;
            
            float* outL = outputBuffer.getWritePointer (0, startSample);
            float* outR = outputBuffer.getNumChannels() > 1 ? outputBuffer.getWritePointer (1, startSample) : nullptr;
            auto procBufPointers = mProcessBuffer.getArrayOfWritePointers();
            // resample and loop source sample
            for (int i=0;i<numSamples;++i)
            {
                auto pos = (int) mSourceSamplePosition;
                auto alpha = (float) (mSourceSamplePosition - pos);
                auto invAlpha = 1.0f - alpha;
                
                // just using a very simple linear interpolation here..
                float l = (inL[pos] * invAlpha + inL[pos + 1] * alpha);
                float r = (inR != nullptr) ? (inR[pos] * invAlpha + inR[pos + 1] * alpha)
                : l;
                
                procBufPointers[0][i] = l * mNoteVelocity;
                procBufPointers[1][i] = r * mNoteVelocity;
                mSourceSamplePosition += mPitchRatio;
                if (mLoopSample == false)
                {
                    if (mSourceSamplePosition > playingSound->getSampleLength())
                    {
                        stopNote (0.0f, false);
                        break;
                    }
                } else
                {
                    if (mSourceSamplePosition > playingSound->getSampleLength())
                    {
                        mSourceSamplePosition = 0.0;
                    }
                }
            }
            // do post processing on resampled buffer
            renderVoicePostProcessing(mProcessBuffer, numSamples);
            // sum post processed audio into the voice output buffer
            for (int i=0;i<numSamples;++i)
            {
                if (outR != nullptr)
                {
                    outL[i] += procBufPointers[0][i];
                    outR[i] += procBufPointers[1][i];
                    //*outL++ += l;
                    //*outR++ += r;
                }
                else
                {
                    //*outL++ += (l + r) * 0.5f;
                }
            }
            
        }
    }
    // Can override this to implement envelope, filter, pan...
    // Note that it is *not* necessary to sum into the buffer, you
    // can just replace the contents
    virtual void renderVoicePostProcessing(juce::AudioBuffer<float>& buf, int numSamples)
    {
        
    }
    // Can override these to do custom behavior on note start/stop, like start/stop ADSR envelope
    virtual void onStartNote(int midiNoteNumber, float velocity, int pitchWheel) {}
    virtual void onStopNote(float velocity, bool allowNoteOff) {}
protected:
    double mPitchRatio = 1.0;
    double mSourceSamplePosition = 0.0;
    float mNoteVelocity = 0.0f;
    bool mLoopSample = true;
    SamplerSoundEx* mSound = nullptr;
    juce::AudioBuffer<float> mProcessBuffer;
};

class SamplerVoiceWithEnvelope : public SamplerVoiceEx
{
public:
    SamplerVoiceWithEnvelope() : SamplerVoiceEx()
    {
        
    }
    void renderVoicePostProcessing(juce::AudioBuffer<float>& buf, int numSamples) override
    {
        mADSR.applyEnvelopeToBuffer(buf, 0, numSamples);
    }
    void onStartNote(int midiNoteNumber, float velocity, int pitchWheel) override
    {
        mADSR.setSampleRate(getSampleRate());
        mADSR.setParameters({1.0,0.5,0.5,2.0});
        mADSR.noteOn();
    }
    void onStopNote(float velocity, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            mADSR.noteOff();
        }
        else
        {
            mADSR.reset();
        }
    }
    juce::ADSR mADSR;
};
