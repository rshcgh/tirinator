#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::CriticalSection gBandLock;
    constexpr float minBandWidthHz = 1.0f;
    constexpr float uiLowFrequencyHz = 20.0f;
    constexpr int firstPrecisionIndex = 2; // quarter-note grid
    constexpr uint32_t defaultAllOnMask = 0xFu;
    constexpr std::array<int, TirinatorAudioProcessor::precisionCount> kPrecisionSteps{ 1, 2, 4, 8, 16, 32 };
    constexpr std::array<const char*, TirinatorAudioProcessor::precisionCount> kPrecisionNames
    {
        "whole",
        "half",
        "quarter",
        "eighth",
        "16th",
        "32nd"
    };

    PatternSpec defaultPatternSpec(float onVelocity = 1.0f, float offVelocity = 0.0f, float stereoPosition = 0.5f)
    {
        PatternSpec pattern;
        pattern.precisionIndex = firstPrecisionIndex;
        pattern.patternMask = defaultAllOnMask;
        pattern.onVelocity = onVelocity;
        pattern.offVelocity = offVelocity;
        pattern.stereoPosition = stereoPosition;
        return pattern;
    }

    BandSpec defaultBandSpec(float onVelocity = 1.0f, float offVelocity = 0.0f, float stereoPosition = 0.5f)
    {
        BandSpec band;
        band.lowHz = uiLowFrequencyHz;
        band.highHz = 20000.0f;
        band.frequencyLocked = false;
        band.patterns.clear();
        band.patterns.push_back(defaultPatternSpec(onVelocity, offVelocity, stereoPosition));
        return band;
    }

    PatternSpec sanitisePattern(PatternSpec pattern)
    {
        pattern.precisionIndex = juce::jlimit(0, TirinatorAudioProcessor::precisionCount - 1, pattern.precisionIndex);
        const int stepCount = TirinatorAudioProcessor::getStepCountForPrecision(pattern.precisionIndex);
        const uint32_t validMask = TirinatorAudioProcessor::maskForStepCount(stepCount);
        pattern.patternMask &= validMask;
        pattern.onVelocity = juce::jlimit(0.0f, 1.0f, pattern.onVelocity);
        pattern.offVelocity = juce::jlimit(0.0f, 1.0f, pattern.offVelocity);
        pattern.stereoPosition = juce::jlimit(0.0f, 1.0f, pattern.stereoPosition);
        return pattern;
    }

    juce::XmlElement* createBandsXml(const char* tag, const std::vector<BandSpec>& bands)
    {
        auto* root = new juce::XmlElement(tag);

        for (size_t i = 0; i < bands.size(); ++i)
        {
            const auto& band = bands[i];
            auto* child = root->createNewChildElement("Band");
            child->setAttribute("index", (int)i);
            child->setAttribute("lowHz", band.lowHz);
            child->setAttribute("highHz", band.highHz);
            child->setAttribute("frequencyLocked", band.frequencyLocked ? 1 : 0);

            for (size_t j = 0; j < band.patterns.size(); ++j)
            {
                const auto& pattern = band.patterns[j];
                auto* patternElement = child->createNewChildElement("Pattern");
                patternElement->setAttribute("index", (int)j);
                patternElement->setAttribute("precisionIndex", pattern.precisionIndex);
                patternElement->setAttribute("patternMask", (int)pattern.patternMask);
                patternElement->setAttribute("onVelocity", pattern.onVelocity);
                patternElement->setAttribute("offVelocity", pattern.offVelocity);
                patternElement->setAttribute("stereoPosition", pattern.stereoPosition);
            }
        }

        return root;
    }

    std::vector<BandSpec> readBandsFromXml(const juce::XmlElement& root)
    {
        std::vector<BandSpec> bands;

        forEachXmlChildElementWithTagName(root, e, "Band")
        {
            BandSpec band;
            band.lowHz = (float)e->getDoubleAttribute("lowHz", uiLowFrequencyHz);
            band.highHz = (float)e->getDoubleAttribute("highHz", 20000.0);
            band.frequencyLocked = e->getBoolAttribute("frequencyLocked", false);
            band.patterns.clear();

            bool foundPattern = false;
            forEachXmlChildElementWithTagName(*e, p, "Pattern")
            {
                PatternSpec pattern;
                pattern.precisionIndex = p->getIntAttribute("precisionIndex", firstPrecisionIndex);
                pattern.patternMask = (uint32_t)p->getIntAttribute("patternMask", (int)defaultAllOnMask);
                pattern.onVelocity = (float)p->getDoubleAttribute("onVelocity", 1.0);
                pattern.offVelocity = (float)p->getDoubleAttribute("offVelocity", 0.0);
                pattern.stereoPosition = (float)p->getDoubleAttribute("stereoPosition", 0.5);
                band.patterns.push_back(pattern);
                foundPattern = true;
            }

            if (!foundPattern)
                band.patterns.push_back(defaultPatternSpec());

            bands.push_back(band);
        }

        return bands;
    }

    inline float panToLeftGain(float pan)
    {
        pan = juce::jlimit(0.0f, 1.0f, pan);
        return std::cos(pan * juce::MathConstants<float>::halfPi);
    }

    inline float panToRightGain(float pan)
    {
        pan = juce::jlimit(0.0f, 1.0f, pan);
        return std::sin(pan * juce::MathConstants<float>::halfPi);
    }

    BandSpec sanitiseBand(BandSpec band, double sampleRate)
    {
        const float nyquist = static_cast<float> (0.5 * sampleRate);
        const float maxLow = std::max(0.0f, nyquist - minBandWidthHz);

        band.lowHz = juce::jlimit(0.0f, maxLow, band.lowHz);
        band.highHz = juce::jlimit(band.lowHz + minBandWidthHz, nyquist, band.highHz);

        if (band.highHz <= band.lowHz)
            band.highHz = juce::jmin(nyquist, band.lowHz + minBandWidthHz);

        if (band.highHz <= band.lowHz)
            band.highHz = juce::jmin(nyquist, band.lowHz + 1.0f);

        if (band.highHz <= band.lowHz)
        {
            band.lowHz = 0.0f;
            band.highHz = juce::jmin(nyquist, 1.0f);
        }

        if (band.patterns.empty())
            band.patterns.push_back(defaultPatternSpec());

        for (auto& pattern : band.patterns)
            pattern = sanitisePattern(pattern);

        return band;
    }

    std::vector<BandSpec> rebalanceUnlockedBands(std::vector<BandSpec> bands, double sampleRate)
    {
        if (bands.empty())
            return bands;

        const float lowBound = uiLowFrequencyHz;
        const float highBound = juce::jmin(20000.0f, static_cast<float> (0.5 * sampleRate));

        std::vector<int> lockedIndices;
        lockedIndices.reserve(bands.size());

        for (int i = 0; i < static_cast<int>(bands.size()); ++i)
            if (bands[(size_t)i].frequencyLocked)
                lockedIndices.push_back(i);

        if (lockedIndices.empty())
        {
            const int count = juce::jmax(1, static_cast<int>(bands.size()));
            const float totalWidth = juce::jmax(minBandWidthHz, highBound - lowBound);
            const float stepWidth = totalWidth / static_cast<float>(count);

            for (int i = 0; i < count; ++i)
            {
                auto& band = bands[(size_t)i];
                band.lowHz = lowBound + stepWidth * static_cast<float>(i);
                band.highHz = (i == count - 1) ? highBound
                    : juce::jmin(highBound, band.lowHz + stepWidth);
                band.frequencyLocked = false;
            }

            return bands;
        }

        struct Segment { float low = 0.0f; float high = 0.0f; };

        std::vector<std::pair<float, float>> occupied;
        occupied.reserve(lockedIndices.size());

        for (int idx : lockedIndices)
        {
            auto band = bands[(size_t)idx];
            band = sanitiseBand(band, sampleRate);
            occupied.push_back({ band.lowHz, band.highHz });
        }

        std::sort(occupied.begin(), occupied.end(), [](const auto& a, const auto& b)
            {
                if (a.first == b.first)
                    return a.second < b.second;
                return a.first < b.first;
            });

        std::vector<Segment> freeSegments;
        float cursor = lowBound;

        for (const auto& occ : occupied)
        {
            const float occLow = juce::jlimit(lowBound, highBound, occ.first);
            const float occHigh = juce::jlimit(lowBound, highBound, juce::jmax(occ.first, occ.second));

            if (occLow > cursor + 0.0001f)
                freeSegments.push_back({ cursor, occLow });

            cursor = juce::jmax(cursor, occHigh);
        }

        if (cursor < highBound - 0.0001f)
            freeSegments.push_back({ cursor, highBound });

        std::vector<int> unlockedIndices;
        unlockedIndices.reserve(bands.size());

        for (int i = 0; i < static_cast<int>(bands.size()); ++i)
            if (!bands[(size_t)i].frequencyLocked)
                unlockedIndices.push_back(i);

        if (unlockedIndices.empty() || freeSegments.empty())
            return bands;

        const float totalFreeWidth = [&]
            {
                float sum = 0.0f;
                for (const auto& seg : freeSegments)
                    sum += juce::jmax(0.0f, seg.high - seg.low);
                return sum;
            }();

        if (totalFreeWidth <= 0.0f)
            return bands;

        std::vector<int> counts(freeSegments.size(), 0);
        int remainingBands = static_cast<int> (unlockedIndices.size());
        float remainingWidth = totalFreeWidth;

        for (size_t segIndex = 0; segIndex < freeSegments.size(); ++segIndex)
        {
            const float width = juce::jmax(0.0f, freeSegments[segIndex].high - freeSegments[segIndex].low);

            if (segIndex == freeSegments.size() - 1)
            {
                counts[segIndex] = remainingBands;
            }
            else if (remainingBands > 0 && remainingWidth > 0.0f)
            {
                const float ratio = width / remainingWidth;
                int proposed = static_cast<int> (std::floor(ratio * static_cast<float> (remainingBands)));
                proposed = juce::jlimit(0, remainingBands, proposed);
                counts[segIndex] = proposed;
                remainingBands -= proposed;
                remainingWidth -= width;
            }
        }

        for (size_t segIndex = 0; segIndex < freeSegments.size() && remainingBands > 0; ++segIndex)
        {
            ++counts[segIndex];
            --remainingBands;
        }

        size_t unlockedCursor = 0;

        for (size_t segIndex = 0; segIndex < freeSegments.size(); ++segIndex)
        {
            const int count = counts[segIndex];
            if (count <= 0)
                continue;

            const float segLow = freeSegments[segIndex].low;
            const float segHigh = freeSegments[segIndex].high;
            const float segWidth = juce::jmax(minBandWidthHz, segHigh - segLow);

            for (int bandInSegment = 0; bandInSegment < count && unlockedCursor < unlockedIndices.size(); ++bandInSegment)
            {
                const int bandIndex = unlockedIndices[unlockedCursor++];
                const float from = segLow + segWidth * static_cast<float>(bandInSegment) / static_cast<float>(count);
                const float to = (bandInSegment == count - 1)
                    ? segHigh
                    : segLow + segWidth * static_cast<float>(bandInSegment + 1) / static_cast<float>(count);

                auto& band = bands[(size_t)bandIndex];
                band.lowHz = juce::jlimit(lowBound, highBound, from);
                band.highHz = juce::jlimit(band.lowHz + minBandWidthHz, highBound, to);
                if (band.highHz <= band.lowHz)
                    band.highHz = juce::jmin(highBound, band.lowHz + minBandWidthHz);
            }
        }

        return bands;
    }

    PatternSpec defaultPatternForCategory(TirinatorAudioProcessor::PatternCategory category,
        float defaultOnVelocity,
        float defaultOffVelocity,
        float defaultStereoPosition)
    {
        if (category == TirinatorAudioProcessor::PatternCategory::velocity)
            return defaultPatternSpec(defaultOnVelocity, defaultOffVelocity, 0.5f);

        return defaultPatternSpec(defaultOnVelocity, defaultOffVelocity, defaultStereoPosition);
    }

    BandSpec defaultBandForCategory(TirinatorAudioProcessor::PatternCategory category,
        float defaultOnVelocity,
        float defaultOffVelocity,
        float defaultStereoPosition)
    {
        auto band = defaultBandSpec(defaultOnVelocity, defaultOffVelocity, defaultStereoPosition);
        if (category == TirinatorAudioProcessor::PatternCategory::velocity)
        {
            for (auto& pattern : band.patterns)
                pattern.stereoPosition = 0.5f;
        }
        return band;
    }

    TirinatorAudioProcessor::PatternCategory legacyCategory()
    {
        return TirinatorAudioProcessor::PatternCategory::velocity;
    }
}

void TirinatorAudioProcessor::BandRuntime::prepare(double sampleRate, int numChannels, const BandSpec& spec)
{
    lowHz = spec.lowHz;
    highHz = spec.highHz;
    frequencyLocked = spec.frequencyLocked;

    patterns.clear();
    patterns.reserve(spec.patterns.size());

    for (const auto& pattern : spec.patterns)
        patterns.push_back(PatternRuntime{ juce::jlimit(0, TirinatorAudioProcessor::precisionCount - 1, pattern.precisionIndex),
                                             pattern.patternMask,
                                             juce::jlimit(0.0f, 1.0f, pattern.onVelocity),
                                             juce::jlimit(0.0f, 1.0f, pattern.offVelocity),
                                             juce::jlimit(0.0f, 1.0f, pattern.stereoPosition) });

    highPassFilters.resize(static_cast<size_t> (juce::jmax(0, numChannels)));
    lowPassFilters.resize(static_cast<size_t> (juce::jmax(0, numChannels)));

    for (int ch = 0; ch < numChannels; ++ch)
    {
        highPassFilters[(size_t)ch].setHighPass(sampleRate, lowHz);
        lowPassFilters[(size_t)ch].setLowPass(sampleRate, highHz);
        highPassFilters[(size_t)ch].reset();
        lowPassFilters[(size_t)ch].reset();
    }
}

void TirinatorAudioProcessor::BandRuntime::reset()
{
    for (auto& f : highPassFilters)
        f.reset();

    for (auto& f : lowPassFilters)
        f.reset();
}

float TirinatorAudioProcessor::BandRuntime::processSample(int channel, float input) const
{
    if (channel < 0)
        return 0.0f;

    const auto idx = static_cast<size_t>(channel);
    if (idx >= highPassFilters.size() || idx >= lowPassFilters.size())
        return input;

    auto x = highPassFilters[idx].process(input);
    x = lowPassFilters[idx].process(x);
    return x;
}

float TirinatorAudioProcessor::BandRuntime::gainAtBeat(double beatPosition) const
{
    if (patterns.empty())
        return 1.0f;

    const double wholeNotePosition = beatPosition / 4.0;
    const double loopLengthWholeNotes = static_cast<double> (patterns.size());
    const double loopPhase = wholeNotePosition - std::floor(wholeNotePosition / loopLengthWholeNotes) * loopLengthWholeNotes;
    const int patternIndex = juce::jlimit(0, static_cast<int> (patterns.size()) - 1,
        static_cast<int> (std::floor(loopPhase)));

    const auto& pattern = patterns[(size_t)patternIndex];
    const int stepCount = TirinatorAudioProcessor::getStepCountForPrecision(pattern.precisionIndex);
    if (stepCount <= 0)
        return juce::jlimit(0.0f, 1.0f, pattern.onVelocity);

    const double withinWholeNote = loopPhase - static_cast<double> (patternIndex);
    int stepIndex = static_cast<int> (std::floor(withinWholeNote * static_cast<double> (stepCount)));
    stepIndex = juce::jlimit(0, stepCount - 1, stepIndex);

    const bool stepOn = (pattern.patternMask & (1u << stepIndex)) != 0;
    return juce::jlimit(0.0f, 1.0f, stepOn ? pattern.onVelocity : pattern.offVelocity);
}

float TirinatorAudioProcessor::BandRuntime::stereoPositionAtBeat(double beatPosition) const
{
    if (patterns.empty())
        return 0.5f;

    const double wholeNotePosition = beatPosition / 4.0;
    const double loopLengthWholeNotes = static_cast<double> (patterns.size());
    const double loopPhase = wholeNotePosition - std::floor(wholeNotePosition / loopLengthWholeNotes) * loopLengthWholeNotes;
    const int patternIndex = juce::jlimit(0, static_cast<int> (patterns.size()) - 1,
        static_cast<int> (std::floor(loopPhase)));

    const auto& pattern = patterns[(size_t)patternIndex];
    const int stepCount = TirinatorAudioProcessor::getStepCountForPrecision(pattern.precisionIndex);
    if (stepCount <= 0)
        return 0.5f;

    const double withinWholeNote = loopPhase - static_cast<double> (patternIndex);
    int stepIndex = static_cast<int> (std::floor(withinWholeNote * static_cast<double> (stepCount)));
    stepIndex = juce::jlimit(0, stepCount - 1, stepIndex);

    const bool stepOn = (pattern.patternMask & (1u << stepIndex)) != 0;
    return stepOn ? juce::jlimit(0.0f, 1.0f, pattern.stereoPosition) : 0.5f;
}

TirinatorAudioProcessor::TirinatorAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
#endif
{
    auto velocityBands = std::make_shared<std::vector<BandSpec>>();
    velocityBands->push_back(defaultBandForCategory(PatternCategory::velocity, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));

    auto stereoBands = std::make_shared<std::vector<BandSpec>>();
    stereoBands->push_back(defaultBandForCategory(PatternCategory::stereo, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));

    const juce::ScopedLock sl(gBandLock);
    this->velocityBandConfig = velocityBands;
    this->stereoBandConfig = stereoBands;
    ++velocityConfigRevision;
    ++stereoConfigRevision;
}

TirinatorAudioProcessor::~TirinatorAudioProcessor() = default;

const juce::String TirinatorAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool TirinatorAudioProcessor::acceptsMidi() const
{
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool TirinatorAudioProcessor::producesMidi() const
{
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool TirinatorAudioProcessor::isMidiEffect() const
{
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double TirinatorAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int TirinatorAudioProcessor::getNumPrograms()
{
    return 1;
}

int TirinatorAudioProcessor::getCurrentProgram()
{
    return 0;
}

void TirinatorAudioProcessor::setCurrentProgram(int)
{
}

const juce::String TirinatorAudioProcessor::getProgramName(int)
{
    return {};
}

void TirinatorAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void TirinatorAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    freeRunBeatPosition = 0.0;
    rebuildRuntimeIfNeeded(getTotalNumOutputChannels(), sampleRate);
}

void TirinatorAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool TirinatorAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;
#endif

    return true;
#endif
}
#endif

std::shared_ptr<std::vector<BandSpec>> TirinatorAudioProcessor::copyCurrentBands(PatternCategory category) const
{
    const juce::ScopedLock sl(gBandLock);
    return category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig;
}

int TirinatorAudioProcessor::getBandCount(PatternCategory category) const
{
    const juce::ScopedLock sl(gBandLock);
    const auto& config = (category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig);
    return config ? static_cast<int> (config->size()) : 0;
}

int TirinatorAudioProcessor::getBandCount() const
{
    return getBandCount(legacyCategory());
}

std::vector<BandSpec> TirinatorAudioProcessor::getBandSpecs(PatternCategory category) const
{
    const juce::ScopedLock sl(gBandLock);
    const auto& config = (category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig);
    if (config == nullptr)
        return {};
    return *config;
}

std::vector<BandSpec> TirinatorAudioProcessor::getBandSpecs() const
{
    return getBandSpecs(legacyCategory());
}

BandSpec TirinatorAudioProcessor::getBandSpec(PatternCategory category, int index) const
{
    const juce::ScopedLock sl(gBandLock);
    const auto& config = (category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig);

    if (config == nullptr || index < 0 || index >= static_cast<int>(config->size()))
        return {};

    return (*config)[(size_t)index];
}

BandSpec TirinatorAudioProcessor::getBandSpec(int index) const
{
    return getBandSpec(legacyCategory(), index);
}

int TirinatorAudioProcessor::getPatternCount(PatternCategory category, int bandIndex) const
{
    const juce::ScopedLock sl(gBandLock);
    const auto& config = (category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig);

    if (config == nullptr || bandIndex < 0 || bandIndex >= static_cast<int>(config->size()))
        return 0;

    return static_cast<int>((*config)[(size_t)bandIndex].patterns.size());
}

int TirinatorAudioProcessor::getPatternCount(int bandIndex) const
{
    return getPatternCount(legacyCategory(), bandIndex);
}

PatternSpec TirinatorAudioProcessor::getPatternSpec(PatternCategory category, int bandIndex, int patternIndex) const
{
    const juce::ScopedLock sl(gBandLock);
    const auto& config = (category == PatternCategory::velocity ? velocityBandConfig : stereoBandConfig);

    if (config == nullptr
        || bandIndex < 0
        || bandIndex >= static_cast<int>(config->size())
        || patternIndex < 0
        || patternIndex >= static_cast<int>((*config)[(size_t)bandIndex].patterns.size()))
    {
        return defaultPatternSpec(defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue);
    }

    return (*config)[(size_t)bandIndex].patterns[(size_t)patternIndex];
}

PatternSpec TirinatorAudioProcessor::getPatternSpec(int bandIndex, int patternIndex) const
{
    return getPatternSpec(legacyCategory(), bandIndex, patternIndex);
}

void TirinatorAudioProcessor::normaliseAndStoreBands(PatternCategory category, std::vector<BandSpec> bands, bool rebalanceUnlockedBands)
{
    const double sr = currentSampleRate > 0.0 ? currentSampleRate : 44100.0;

    if (rebalanceUnlockedBands)
        bands = ::rebalanceUnlockedBands(std::move(bands), sr);

    for (auto& band : bands)
        band = sanitiseBand(band, sr);

    const juce::ScopedLock sl(gBandLock);

    if (category == PatternCategory::velocity)
    {
        velocityBandConfig = std::make_shared<std::vector<BandSpec>>(std::move(bands));
        ++velocityConfigRevision;
    }
    else
    {
        stereoBandConfig = std::make_shared<std::vector<BandSpec>>(std::move(bands));
        ++stereoConfigRevision;
    }
}

void TirinatorAudioProcessor::setBandCount(PatternCategory category, int newCount)
{
    newCount = juce::jlimit(0, maxUniqueBands, newCount);

    auto bands = copyCurrentBands(category);
    std::vector<BandSpec> next;
    if (bands != nullptr)
        next = *bands;

    if (newCount > static_cast<int> (next.size()))
    {
        const int oldCount = static_cast<int> (next.size());
        next.resize((size_t)newCount, defaultBandForCategory(category, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));

        for (int i = oldCount; i < newCount; ++i)
            next[(size_t)i].frequencyLocked = false;
    }
    else if (newCount < static_cast<int>(next.size()))
    {
        next.resize((size_t)newCount);
    }

    normaliseAndStoreBands(category, std::move(next), true);
}

void TirinatorAudioProcessor::setBandCount(int newCount)
{
    setBandCount(legacyCategory(), newCount);
}

void TirinatorAudioProcessor::removeBand(PatternCategory category, int index)
{
    auto bands = copyCurrentBands(category);
    if (!bands || index < 0 || index >= static_cast<int>(bands->size()))
        return;

    auto next = *bands;
    next.erase(next.begin() + index);
    normaliseAndStoreBands(category, std::move(next), true);
}

void TirinatorAudioProcessor::removeBand(int index)
{
    removeBand(legacyCategory(), index);
}

void TirinatorAudioProcessor::setBandSpec(PatternCategory category, int index, float lowHz, float highHz)
{
    auto bands = copyCurrentBands(category);
    if (!bands)
        bands = std::make_shared<std::vector<BandSpec>>();

    if (index < 0)
        return;

    if (index >= static_cast<int>(bands->size()))
        bands->resize((size_t)index + 1, defaultBandForCategory(category, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));

    (*bands)[(size_t)index].lowHz = lowHz;
    (*bands)[(size_t)index].highHz = highHz;
    (*bands)[(size_t)index].frequencyLocked = true;

    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::setBandSpec(int index, float lowHz, float highHz)
{
    setBandSpec(legacyCategory(), index, lowHz, highHz);
}

void TirinatorAudioProcessor::setBandPatternCount(PatternCategory category, int bandIndex, int newPatternCount)
{
    auto bands = copyCurrentBands(category);
    if (!bands || bandIndex < 0 || bandIndex >= static_cast<int>(bands->size()))
        return;

    auto& patterns = (*bands)[(size_t)bandIndex].patterns;
    newPatternCount = juce::jlimit(1, 32, newPatternCount);
    patterns.resize((size_t)newPatternCount, defaultPatternForCategory(category, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));

    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::setBandPatternCount(int bandIndex, int newPatternCount)
{
    setBandPatternCount(legacyCategory(), bandIndex, newPatternCount);
}

void TirinatorAudioProcessor::setBandPatternSpec(PatternCategory category, int bandIndex, int patternIndex, int precisionIndex, uint32_t patternMask, float onVelocity, float offVelocity, float stereoPosition)
{
    auto bands = copyCurrentBands(category);
    if (!bands
        || bandIndex < 0
        || bandIndex >= static_cast<int>(bands->size())
        || patternIndex < 0
        || patternIndex >= static_cast<int>((*bands)[(size_t)bandIndex].patterns.size()))
    {
        return;
    }

    auto& pattern = (*bands)[(size_t)bandIndex].patterns[(size_t)patternIndex];
    pattern.precisionIndex = juce::jlimit(0, precisionCount - 1, precisionIndex);
    const int stepCount = getStepCountForPrecision(pattern.precisionIndex);
    const uint32_t validMask = maskForStepCount(stepCount);
    pattern.patternMask = patternMask & validMask;
    pattern.onVelocity = juce::jlimit(0.0f, 1.0f, onVelocity);
    pattern.offVelocity = juce::jlimit(0.0f, 1.0f, offVelocity);
    pattern.stereoPosition = juce::jlimit(0.0f, 1.0f, stereoPosition);

    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::setBandPatternSpec(int bandIndex, int patternIndex, int precisionIndex, uint32_t patternMask, float onVelocity, float offVelocity, float stereoPosition)
{
    setBandPatternSpec(legacyCategory(), bandIndex, patternIndex, precisionIndex, patternMask, onVelocity, offVelocity, stereoPosition);
}

void TirinatorAudioProcessor::addBandPattern(PatternCategory category, int bandIndex)
{
    auto bands = copyCurrentBands(category);
    if (!bands || bandIndex < 0 || bandIndex >= static_cast<int>(bands->size()))
        return;

    (*bands)[(size_t)bandIndex].patterns.push_back(defaultPatternForCategory(category, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));
    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::addBandPattern(int bandIndex)
{
    addBandPattern(legacyCategory(), bandIndex);
}

void TirinatorAudioProcessor::copyAndAddBelowBandPattern(PatternCategory category, int bandIndex, int patternIndex)
{
    auto bands = copyCurrentBands(category);
    if (!bands
        || bandIndex < 0
        || bandIndex >= static_cast<int>(bands->size()))
    {
        return;
    }

    auto& patterns = (*bands)[(size_t)bandIndex].patterns;
    if (patternIndex < 0 || patternIndex >= static_cast<int>(patterns.size()))
        return;

    const auto copied = patterns[(size_t)patternIndex];
    patterns.insert(patterns.begin() + patternIndex + 1, copied);
    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::copyAndAddBelowBandPattern(int bandIndex, int patternIndex)
{
    copyAndAddBelowBandPattern(legacyCategory(), bandIndex, patternIndex);
}

void TirinatorAudioProcessor::removeBandPattern(PatternCategory category, int bandIndex, int patternIndex)
{
    auto bands = copyCurrentBands(category);
    if (!bands
        || bandIndex < 0
        || bandIndex >= static_cast<int>(bands->size()))
    {
        return;
    }

    auto& patterns = (*bands)[(size_t)bandIndex].patterns;
    if (patternIndex < 0 || patternIndex >= static_cast<int>(patterns.size()))
        return;

    if (patterns.size() <= 1)
    {
        patterns.clear();
        patterns.push_back(defaultPatternForCategory(category, defaultOnVelocityValue, defaultOffVelocityValue, defaultStereoPositionValue));
    }
    else
    {
        patterns.erase(patterns.begin() + patternIndex);
    }

    normaliseAndStoreBands(category, *bands, false);
}

void TirinatorAudioProcessor::removeBandPattern(int bandIndex, int patternIndex)
{
    removeBandPattern(legacyCategory(), bandIndex, patternIndex);
}

void TirinatorAudioProcessor::replaceBands(PatternCategory category, std::vector<BandSpec> bands)
{
    if (bands.size() > (size_t)maxUniqueBands)
        bands.resize((size_t)maxUniqueBands);

    normaliseAndStoreBands(category, std::move(bands), false);
}

void TirinatorAudioProcessor::replaceBands(std::vector<BandSpec> bands)
{
    replaceBands(legacyCategory(), std::move(bands));
}

void TirinatorAudioProcessor::setBypassed(bool shouldBypass)
{
    const juce::ScopedLock sl(gBandLock);
    bypassed = shouldBypass;
}

bool TirinatorAudioProcessor::isBypassed() const
{
    const juce::ScopedLock sl(gBandLock);
    return bypassed;
}

float TirinatorAudioProcessor::getDefaultOnVelocity() const
{
    const juce::ScopedLock sl(gBandLock);
    return defaultOnVelocityValue;
}

float TirinatorAudioProcessor::getDefaultOffVelocity() const
{
    const juce::ScopedLock sl(gBandLock);
    return defaultOffVelocityValue;
}

float TirinatorAudioProcessor::getDefaultStereoPosition() const
{
    const juce::ScopedLock sl(gBandLock);
    return defaultStereoPositionValue;
}

bool TirinatorAudioProcessor::isSpectralViewEnabled() const
{
    const juce::ScopedLock sl(gBandLock);
    return spectralViewEnabledValue;
}

void TirinatorAudioProcessor::setSpectralViewEnabled(bool enabled)
{
    const juce::ScopedLock sl(gBandLock);
    spectralViewEnabledValue = enabled;
}

void TirinatorAudioProcessor::setDefaultOnVelocity(float value)
{
    const juce::ScopedLock sl(gBandLock);
    defaultOnVelocityValue = juce::jlimit(0.0f, 1.0f, value);
}

void TirinatorAudioProcessor::setDefaultOffVelocity(float value)
{
    const juce::ScopedLock sl(gBandLock);
    defaultOffVelocityValue = juce::jlimit(0.0f, 1.0f, value);
}

void TirinatorAudioProcessor::setDefaultStereoPosition(float value)
{
    const juce::ScopedLock sl(gBandLock);
    defaultStereoPositionValue = juce::jlimit(0.0f, 1.0f, value);
}

void TirinatorAudioProcessor::rebuildRuntimeIfNeeded(int numChannels, double sampleRate)
{
    std::shared_ptr<std::vector<BandSpec>> velocityBands;
    std::shared_ptr<std::vector<BandSpec>> stereoBands;
    uint64_t velocityRevision = 0;
    uint64_t stereoRevision = 0;

    {
        const juce::ScopedLock sl(gBandLock);
        velocityBands = velocityBandConfig;
        stereoBands = stereoBandConfig;
        velocityRevision = velocityConfigRevision;
        stereoRevision = stereoConfigRevision;
    }

    const bool velocityNeedsRebuild = (velocityRevision != velocityRuntimeRevision)
        || (numChannels != currentNumChannels)
        || (sampleRate != currentSampleRate)
        || ((velocityBands != nullptr) && velocityRuntimeBands.size() != velocityBands->size())
        || ((velocityBands == nullptr) && !velocityRuntimeBands.empty());

    const bool stereoNeedsRebuild = (stereoRevision != stereoRuntimeRevision)
        || (numChannels != currentNumChannels)
        || (sampleRate != currentSampleRate)
        || ((stereoBands != nullptr) && stereoRuntimeBands.size() != stereoBands->size())
        || ((stereoBands == nullptr) && !stereoRuntimeBands.empty());

    if (!velocityNeedsRebuild && !stereoNeedsRebuild)
        return;

    std::vector<BandRuntime> rebuiltVelocity;
    std::vector<BandRuntime> rebuiltStereo;

    if (velocityBands)
        rebuiltVelocity.resize(velocityBands->size());
    if (stereoBands)
        rebuiltStereo.resize(stereoBands->size());

    for (size_t i = 0; i < rebuiltVelocity.size(); ++i)
        rebuiltVelocity[i].prepare(sampleRate, numChannels, (*velocityBands)[i]);

    for (size_t i = 0; i < rebuiltStereo.size(); ++i)
        rebuiltStereo[i].prepare(sampleRate, numChannels, (*stereoBands)[i]);

    const juce::ScopedLock sl(gBandLock);
    velocityRuntimeBands = std::move(rebuiltVelocity);
    stereoRuntimeBands = std::move(rebuiltStereo);
    velocityRuntimeRevision = velocityRevision;
    stereoRuntimeRevision = stereoRevision;
    currentNumChannels = numChannels;
    currentSampleRate = sampleRate;
}

void TirinatorAudioProcessor::renderRange(juce::AudioBuffer<float>& output,
    const juce::AudioBuffer<float>& input,
    int startSample,
    int endSample,
    int numChannels,
    double startingBeatPosition,
    double beatsPerSample,
    std::vector<BandRuntime>& runtimeBands,
    bool applyGain,
    bool applyStereo)
{
    startSample = juce::jlimit(0, input.getNumSamples(), startSample);
    endSample = juce::jlimit(startSample, input.getNumSamples(), endSample);

    if (startSample >= endSample || numChannels <= 0)
        return;

    const int inputChannels = input.getNumChannels();

    for (int sample = startSample; sample < endSample; ++sample)
    {
        const double beatPosition = startingBeatPosition + static_cast<double>(sample - startSample) * beatsPerSample;

        float monoSum = 0.0f;
        float leftSum = 0.0f;
        float rightSum = 0.0f;

        for (auto& band : runtimeBands)
        {
            const float gain = applyGain ? band.gainAtBeat(beatPosition) : 1.0f;
            if (gain == 0.0f)
                continue;

            const float stereoPosition = applyStereo ? band.stereoPositionAtBeat(beatPosition) : 0.5f;
            const float leftGain = panToLeftGain(stereoPosition);
            const float rightGain = panToRightGain(stereoPosition);

            for (int ch = 0; ch < juce::jmin(numChannels, inputChannels); ++ch)
            {
                const float in = input.getSample(ch, sample);
                const float filtered = band.processSample(ch, in) * gain;

                if (numChannels == 1)
                    monoSum += filtered;
                else
                {
                    leftSum += filtered * leftGain;
                    rightSum += filtered * rightGain;
                }
            }
        }

        if (numChannels == 1)
        {
            output.addSample(0, sample, monoSum);
        }
        else
        {
            output.addSample(0, sample, leftSum);
            output.addSample(1, sample, rightSum);
        }
    }
}

void TirinatorAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    const int inputChannelsToCopy = juce::jmin(totalNumInputChannels, buffer.getNumChannels());

    if (numSamples <= 0 || totalNumOutputChannels <= 0)
    {
        buffer.clear();
        return;
    }

    juce::AudioBuffer<float> inputCopy(juce::jmax(1, inputChannelsToCopy), numSamples);
    inputCopy.clear();

    for (int ch = 0; ch < inputChannelsToCopy; ++ch)
        inputCopy.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    buffer.clear();

    for (int ch = 0; ch < juce::jmin(totalNumOutputChannels, inputCopy.getNumChannels()); ++ch)
        buffer.copyFrom(ch, 0, inputCopy, ch, 0, numSamples);

    for (int ch = inputChannelsToCopy; ch < totalNumOutputChannels; ++ch)
        buffer.clear(ch, 0, numSamples);

    bool isBypassedNow = false;
    {
        const juce::ScopedLock sl(gBandLock);
        isBypassedNow = bypassed;
    }

    if (isBypassedNow)
        return;

    const double sampleRate = currentSampleRate > 0.0 ? currentSampleRate : getSampleRate();
    rebuildRuntimeIfNeeded(totalNumOutputChannels, sampleRate);

    juce::AudioPlayHead::CurrentPositionInfo positionInfo;
    auto* hostPlayHead = getPlayHead();
    const bool hasPosition = hostPlayHead != nullptr && hostPlayHead->getCurrentPosition(positionInfo);

    const double bpm = (hasPosition && positionInfo.bpm > 0.0) ? positionInfo.bpm : 120.0;
    const double beatsPerSample = bpm / 60.0 / sampleRate;
    const double startingBeatPosition = hasPosition && positionInfo.ppqPosition >= 0.0
        ? positionInfo.ppqPosition
        : freeRunBeatPosition;

    lastPlaybackBeatPosition.store(startingBeatPosition);

    buffer.clear();
    renderRange(buffer, inputCopy, 0, numSamples, totalNumOutputChannels, startingBeatPosition, beatsPerSample, velocityRuntimeBands, true, false);
    renderRange(buffer, inputCopy, 0, numSamples, totalNumOutputChannels, startingBeatPosition, beatsPerSample, stereoRuntimeBands, true, true);

    if (!hasPosition)
        freeRunBeatPosition = startingBeatPosition + beatsPerSample * static_cast<double> (numSamples);
}

bool TirinatorAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* TirinatorAudioProcessor::createEditor()
{
    return new TirinatorAudioProcessorEditor(*this);
}

void TirinatorAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::XmlElement xml("TirinatorState");

    auto velocityBands = copyCurrentBands(PatternCategory::velocity);
    auto stereoBands = copyCurrentBands(PatternCategory::stereo);
    xml.setAttribute("bypassed", isBypassed() ? 1 : 0);
    xml.setAttribute("defaultOnVelocity", getDefaultOnVelocity());
    xml.setAttribute("defaultOffVelocity", getDefaultOffVelocity());
    xml.setAttribute("defaultStereoPosition", getDefaultStereoPosition());
    xml.setAttribute("spectralViewEnabled", isSpectralViewEnabled() ? 1 : 0);

    if (auto* velXml = createBandsXml("VelocityBands", velocityBands ? *velocityBands : std::vector<BandSpec>{}))
        xml.addChildElement(velXml);

    if (auto* stereoXml = createBandsXml("StereoBands", stereoBands ? *stereoBands : std::vector<BandSpec>{}))
        xml.addChildElement(stereoXml);

    copyXmlToBinary(xml, destData);
}

void TirinatorAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xmlState = getXmlFromBinary(data, sizeInBytes);
    if (xmlState == nullptr)
        return;

    if (xmlState->hasTagName("TirinatorState"))
    {
        {
            const double onDefault = xmlState->getDoubleAttribute("defaultOnVelocity", defaultOnVelocityValue);
            const double offDefault = xmlState->getDoubleAttribute("defaultOffVelocity", defaultOffVelocityValue);
            const double stereoDefault = xmlState->getDoubleAttribute("defaultStereoPosition", defaultStereoPositionValue);
            const bool spectralEnabled = xmlState->getBoolAttribute("spectralViewEnabled", true);
            setDefaultOnVelocity((float)onDefault);
            setDefaultOffVelocity((float)offDefault);
            setDefaultStereoPosition((float)stereoDefault);
            setSpectralViewEnabled(spectralEnabled);
        }

        bool loadedVelocity = false;
        bool loadedStereo = false;

        if (auto* velocityXml = xmlState->getChildByName("VelocityBands"))
        {
            replaceBands(PatternCategory::velocity, readBandsFromXml(*velocityXml));
            loadedVelocity = true;
        }

        if (auto* stereoXml = xmlState->getChildByName("StereoBands"))
        {
            replaceBands(PatternCategory::stereo, readBandsFromXml(*stereoXml));
            loadedStereo = true;
        }

        if (!loadedVelocity && !loadedStereo)
        {
            auto legacyBands = readBandsFromXml(*xmlState);
            if (!legacyBands.empty())
            {
                replaceBands(PatternCategory::velocity, legacyBands);
                replaceBands(PatternCategory::stereo, legacyBands);
            }
        }
    }

    setBypassed(xmlState->getBoolAttribute("bypassed", false));
}

juce::String TirinatorAudioProcessor::getPrecisionName(int precisionIndex)
{
    precisionIndex = juce::jlimit(0, precisionCount - 1, precisionIndex);
    return kPrecisionNames[(size_t)precisionIndex];
}

int TirinatorAudioProcessor::getStepCountForPrecision(int precisionIndex)
{
    precisionIndex = juce::jlimit(0, precisionCount - 1, precisionIndex);
    return kPrecisionSteps[(size_t)precisionIndex];
}

uint32_t TirinatorAudioProcessor::maskForStepCount(int stepCount)
{
    if (stepCount <= 0)
        return 1u;

    if (stepCount >= 32)
        return 0xFFFFFFFFu;

    return (1u << stepCount) - 1u;
}

double TirinatorAudioProcessor::getPlaybackBeatPosition() const
{
    return lastPlaybackBeatPosition.load();
}

void TirinatorAudioProcessor::Biquad::setLowPass(double sampleRate, double frequency, double q)
{
    if (sampleRate <= 0.0 || frequency <= 0.0 || frequency >= sampleRate * 0.5)
    {
        b0 = 1.0;
        b1 = 0.0;
        b2 = 0.0;
        a1 = 0.0;
        a2 = 0.0;
        return;
    }

    const double w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double rawB0 = (1.0 - cosw0) * 0.5;
    const double rawB1 = 1.0 - cosw0;
    const double rawB2 = (1.0 - cosw0) * 0.5;
    const double rawA0 = 1.0 + alpha;
    const double rawA1 = -2.0 * cosw0;
    const double rawA2 = 1.0 - alpha;

    b0 = rawB0 / rawA0;
    b1 = rawB1 / rawA0;
    b2 = rawB2 / rawA0;
    a1 = rawA1 / rawA0;
    a2 = rawA2 / rawA0;
}

void TirinatorAudioProcessor::Biquad::setHighPass(double sampleRate, double frequency, double q)
{
    if (sampleRate <= 0.0 || frequency <= 0.0 || frequency >= sampleRate * 0.5)
    {
        b0 = 1.0;
        b1 = 0.0;
        b2 = 0.0;
        a1 = 0.0;
        a2 = 0.0;
        return;
    }

    const double w0 = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    const double rawB0 = (1.0 + cosw0) * 0.5;
    const double rawB1 = -(1.0 + cosw0);
    const double rawB2 = (1.0 + cosw0) * 0.5;
    const double rawA0 = 1.0 + alpha;
    const double rawA1 = -2.0 * cosw0;
    const double rawA2 = 1.0 - alpha;

    b0 = rawB0 / rawA0;
    b1 = rawB1 / rawA0;
    b2 = rawB2 / rawA0;
    a1 = rawA1 / rawA0;
    a2 = rawA2 / rawA0;
}

float TirinatorAudioProcessor::Biquad::process(float x) const
{
    const float y = static_cast<float> (b0 * x + z1);
    z1 = static_cast<float> (b1 * x - a1 * y + z2);
    z2 = static_cast<float> (b2 * x - a2 * y);
    return y;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TirinatorAudioProcessor();
}