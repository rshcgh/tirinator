#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>

class TirinatorAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Button::Listener,
    private juce::TextEditor::Listener,
    private juce::Timer
{
public:
    explicit TirinatorAudioProcessorEditor(TirinatorAudioProcessor&);
    ~TirinatorAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

public:
    enum class TabMode
    {
        velocity,
        stereo,
        settings,
        info
    };

    struct PatternRow : public juce::Component
    {
        static constexpr int maxSteps = 32;

        PatternRow();
        ~PatternRow() override = default;

        void setIndex(int newIndex);
        void setSpec(const PatternSpec& spec);
        void setMode(TabMode mode);
        void resized() override;
        void paint(juce::Graphics& g) override;

        int getVisibleStepCount() const;
        uint32_t getPatternMask() const;
        int getPrecisionIndex() const;
        float getOnVelocity() const;
        float getOffVelocity() const;
        float getStereoPosition() const;
        juce::ToggleButton* getStepButton(int stepIndex);
        const juce::ToggleButton* getStepButton(int stepIndex) const;

        juce::Label title;
        juce::Label onLabel;
        juce::Label offLabel;
        juce::Label stereoLabel;
        juce::TextEditor onEditor;
        juce::TextEditor offEditor;
        juce::TextEditor stereoEditor;
        juce::TextButton copyButton;
        juce::TextButton randomizeButton;
        juce::TextButton precisionButton;
        juce::TextButton removeButton;
        std::array<std::unique_ptr<juce::ToggleButton>, maxSteps> stepButtons;
        int index = -1;
        int precisionIndex = 2;
        int visibleStepCount = 4;
        uint32_t patternMask = 0xFu;
        float onVelocity = 1.0f;
        float offVelocity = 0.0f;
        float stereoPosition = 0.5f;
        TabMode mode = TabMode::velocity;
    };

    struct BandRow : public juce::Component
    {
        static constexpr int cardRadius = 18;

        BandRow();
        ~BandRow() override = default;

        void setIndex(int newIndex);
        void setSpec(const BandSpec& spec);
        void setMode(TabMode mode);
        int getPatternCount() const;
        int getPreferredHeight() const;
        PatternRow* getPatternRow(int patternIndex);
        const PatternRow* getPatternRow(int patternIndex) const;
        void resized() override;
        void paint(juce::Graphics& g) override;

        juce::Label title;
        juce::Label lowLabel;
        juce::Label highLabel;
        juce::Label patternLabel;
        juce::TextEditor lowEditor;
        juce::TextEditor highEditor;
        juce::TextButton addPatternButton;
        juce::TextButton removeBandButton;
        juce::OwnedArray<PatternRow> patterns;
        int index = -1;
        TabMode mode = TabMode::velocity;
    };

    struct SpectralView : public juce::Component
    {
        SpectralView() = default;

        void setMode(TabMode newMode)
        {
            mode = newMode;
            repaint();
        }

        void setBands(std::vector<BandSpec> newBands)
        {
            bands = std::move(newBands);
            repaint();
        }

        void setBandVolumes(std::vector<float> newBandVolumes)
        {
            bandVolumes = std::move(newBandVolumes);
            repaint();
        }

        void paint(juce::Graphics& g) override;

        TabMode mode = TabMode::velocity;
        std::vector<BandSpec> bands;
        std::vector<float> bandVolumes;
    };

    static TirinatorAudioProcessor::PatternCategory categoryForTab(TabMode mode);

    void buttonClicked(juce::Button* button) override;
    void textEditorTextChanged(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;
    void timerCallback() override;

    void refreshFromProcessor();
    void rebuildRows();
    void layoutRows();
    void setActiveTab(TabMode newMode);
    void syncTabButtons();
    void updateRowModes();
    void updateSpectralViewActivity();
    void applyBandToProcessor(int index);
    void updateBandCountFromEditor();
    void applyPatternToProcessor(int bandIndex, int patternIndex);
    void cyclePatternPrecision(int bandIndex, int patternIndex);
    void applyDefaultVelocities();
    void scheduleRefresh();
    static float parseFloat(const juce::String& text, float fallback);
    static int parseInt(const juce::String& text, int fallback);

    TirinatorAudioProcessor& audioProcessor;
    TabMode activeTab = TabMode::velocity;

    juce::Label titleLabel;
    juce::Label hintLabel;
    juce::Label noBandsLabel;
    juce::TextButton bypassButton;
    juce::TextButton velocityTabButton;
    juce::TextButton stereoTabButton;
    juce::TextButton settingsTabButton;
    juce::TextButton infoTabButton;

    juce::Label countLabel;
    juce::TextEditor countEditor;
    juce::TextButton applyCountButton;
    juce::TextButton addBandButton;
    juce::TextButton removeBandButton;
    SpectralView spectralView;

    juce::Label defaultOnLabel;
    juce::Label defaultOffLabel;
    juce::TextEditor defaultOnEditor;
    juce::TextEditor defaultOffEditor;
    juce::Label defaultStereoLabel;
    juce::TextEditor defaultStereoEditor;
    juce::ToggleButton spectralViewToggle;

    juce::TextEditor infoText;

    juce::Viewport viewport;
    juce::Component rowsContainer;
    juce::OwnedArray<BandRow> rows;

    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TirinatorAudioProcessorEditor)
};