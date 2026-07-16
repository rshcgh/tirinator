#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

struct PatternSpec
{
    int precisionIndex = 2; // 0 = whole, 1 = half, 2 = quarter, ...
    uint32_t patternMask = 0xFu;
    float onVelocity = 1.0f;
    float offVelocity = 0.0f;
    float stereoPosition = 0.5f; // 0 = left, 1 = right
};

struct BandSpec
{
    float lowHz = 20.0f;
    float highHz = 20000.0f;
    bool frequencyLocked = false;
    std::vector<PatternSpec> patterns{ PatternSpec {} };
};

class TirinatorAudioProcessor : public juce::AudioProcessor
{
public:
    enum class PatternCategory
    {
        velocity = 0,
        stereo = 1
    };

    static constexpr int maxUniqueBands = 56;
    static constexpr int precisionCount = 6;

    TirinatorAudioProcessor();
    ~TirinatorAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

#ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
#endif

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    int getBandCount(PatternCategory category) const;
    int getBandCount() const; // legacy: velocity
    std::vector<BandSpec> getBandSpecs(PatternCategory category) const;
    std::vector<BandSpec> getBandSpecs() const;
    BandSpec getBandSpec(PatternCategory category, int index) const;
    BandSpec getBandSpec(int index) const;

    int getPatternCount(PatternCategory category, int bandIndex) const;
    int getPatternCount(int bandIndex) const;
    PatternSpec getPatternSpec(PatternCategory category, int bandIndex, int patternIndex) const;
    PatternSpec getPatternSpec(int bandIndex, int patternIndex) const;

    void setBandCount(PatternCategory category, int newCount);
    void setBandCount(int newCount);
    void removeBand(PatternCategory category, int index);
    void removeBand(int index);
    void setBandSpec(PatternCategory category, int index, float lowHz, float highHz);
    void setBandSpec(int index, float lowHz, float highHz);
    void setBandPatternCount(PatternCategory category, int bandIndex, int newPatternCount);
    void setBandPatternCount(int bandIndex, int newPatternCount);
    void setBandPatternSpec(PatternCategory category, int bandIndex, int patternIndex, int precisionIndex, uint32_t patternMask, float onVelocity, float offVelocity, float stereoPosition);
    void setBandPatternSpec(int bandIndex, int patternIndex, int precisionIndex, uint32_t patternMask, float onVelocity, float offVelocity, float stereoPosition);
    void addBandPattern(PatternCategory category, int bandIndex);
    void addBandPattern(int bandIndex);
    void copyAndAddBelowBandPattern(PatternCategory category, int bandIndex, int patternIndex);
    void copyAndAddBelowBandPattern(int bandIndex, int patternIndex);
    void removeBandPattern(PatternCategory category, int bandIndex, int patternIndex);
    void removeBandPattern(int bandIndex, int patternIndex);
    void replaceBands(PatternCategory category, std::vector<BandSpec> bands);
    void replaceBands(std::vector<BandSpec> bands);

    void setBypassed(bool shouldBypass);
    bool isBypassed() const;

    float getDefaultOnVelocity() const;
    float getDefaultOffVelocity() const;
    float getDefaultStereoPosition() const;
    void setDefaultOnVelocity(float value);
    void setDefaultOffVelocity(float value);
    void setDefaultStereoPosition(float value);
    bool isSpectralViewEnabled() const;
    void setSpectralViewEnabled(bool enabled);

    static juce::String getPrecisionName(int precisionIndex);
    static int getStepCountForPrecision(int precisionIndex);
    static uint32_t maskForStepCount(int stepCount);
    double getPlaybackBeatPosition() const;

private:
    struct Biquad
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        mutable float z1 = 0.0f;
        mutable float z2 = 0.0f;

        void reset()
        {
            z1 = 0.0f;
            z2 = 0.0f;
        }

        void setLowPass(double sampleRate, double frequency, double q = 0.7071067811865476);
        void setHighPass(double sampleRate, double frequency, double q = 0.7071067811865476);
        float process(float x) const;
    };

    struct PatternRuntime
    {
        int precisionIndex = 2;
        uint32_t patternMask = 0xFu;
        float onVelocity = 1.0f;
        float offVelocity = 0.0f;
        float stereoPosition = 0.5f;
    };

    struct BandRuntime
    {
        std::vector<Biquad> highPassFilters;
        std::vector<Biquad> lowPassFilters;
        std::vector<PatternRuntime> patterns;
        float lowHz = 20.0f;
        float highHz = 20000.0f;
        bool frequencyLocked = false;

        void prepare(double sampleRate, int numChannels, const BandSpec& spec);
        void reset();
        float processSample(int channel, float input) const;
        float gainAtBeat(double beatPosition) const;
        float stereoPositionAtBeat(double beatPosition) const;
    };

    std::shared_ptr<std::vector<BandSpec>> velocityBandConfig;
    std::shared_ptr<std::vector<BandSpec>> stereoBandConfig;
    std::vector<BandRuntime> velocityRuntimeBands;
    std::vector<BandRuntime> stereoRuntimeBands;
    double currentSampleRate = 44100.0;
    int currentNumChannels = 0;
    uint64_t velocityConfigRevision = 0;
    uint64_t stereoConfigRevision = 0;
    uint64_t velocityRuntimeRevision = 0;
    uint64_t stereoRuntimeRevision = 0;
    double freeRunBeatPosition = 0.0;
    bool bypassed = false;
    float defaultOnVelocityValue = 1.0f;
    float defaultOffVelocityValue = 0.0f;
    float defaultStereoPositionValue = 0.5f;
    bool spectralViewEnabledValue = true;

    std::atomic<double> lastPlaybackBeatPosition{ 0.0 };

    std::shared_ptr<std::vector<BandSpec>> copyCurrentBands(PatternCategory category) const;
    void normaliseAndStoreBands(PatternCategory category, std::vector<BandSpec> bands, bool rebalanceUnlockedBands);
    void rebuildRuntimeIfNeeded(int numChannels, double sampleRate);
    void renderRange(juce::AudioBuffer<float>& output,
        const juce::AudioBuffer<float>& input,
        int startSample,
        int endSample,
        int numChannels,
        double startingBeatPosition,
        double beatsPerSample,
        std::vector<BandRuntime>& runtimeBands,
        bool applyGain,
        bool applyStereo);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TirinatorAudioProcessor)
};