#include "PluginEditor.h"
#include <cmath>

namespace
{
    constexpr int topPad = 14;
    constexpr int sidePad = 14;
    constexpr int headerHeight = 240;
    constexpr int bandGap = 12;
    constexpr int editorRowHeight = 34;
    constexpr int patternRowHeight = 116;

    juce::Colour backgroundColour() { return juce::Colour(0xff121419); }
    juce::Colour panelColour() { return juce::Colour(0xff1b1f27); }
    juce::Colour accentColour() { return juce::Colour(0xff7aa9ff); }
    juce::Colour outlineColour() { return juce::Colour(0xff465163); }

    constexpr float spectrumMinHz = 20.0f;
    constexpr float spectrumMaxHz = 20000.0f;

    float frequencyToX(float hz, float width)
    {
        hz = juce::jlimit(spectrumMinHz, spectrumMaxHz, hz);
        const auto minLog = std::log10(spectrumMinHz);
        const auto maxLog = std::log10(spectrumMaxHz);
        const auto value = (std::log10(hz) - minLog) / (maxLog - minLog);
        return juce::jlimit(0.0f, width, value * width);
    }

    float bandGainAtBeat(const BandSpec& band, double beatPosition)
    {
        if (band.patterns.empty())
            return 0.0f;

        const double wholeNotePosition = beatPosition / 4.0;
        const double loopLengthWholeNotes = static_cast<double> (band.patterns.size());
        const double loopPhase = wholeNotePosition - std::floor(wholeNotePosition / loopLengthWholeNotes) * loopLengthWholeNotes;
        const int patternIndex = juce::jlimit(0, static_cast<int> (band.patterns.size()) - 1,
            static_cast<int> (std::floor(loopPhase)));

        const auto& pattern = band.patterns[(size_t)patternIndex];
        const int stepCount = TirinatorAudioProcessor::getStepCountForPrecision(pattern.precisionIndex);
        if (stepCount <= 0)
            return juce::jlimit(0.0f, 1.0f, pattern.onVelocity);

        const double withinWholeNote = loopPhase - static_cast<double> (patternIndex);
        int stepIndex = static_cast<int> (std::floor(withinWholeNote * static_cast<double> (stepCount)));
        stepIndex = juce::jlimit(0, stepCount - 1, stepIndex);

        const bool stepOn = (pattern.patternMask & (1u << stepIndex)) != 0;
        return juce::jlimit(0.0f, 1.0f, stepOn ? pattern.onVelocity : pattern.offVelocity);
    }

    class RoundedTextEditorLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor&) override
        {
            auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
            g.setColour(juce::Colour(0xff121419));
            g.fillRoundedRectangle(bounds.reduced(0.5f), 8.0f);
        }

        void drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
        {
            auto bounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
            g.setColour(editor.hasKeyboardFocus(true) ? accentColour() : outlineColour());
            g.drawRoundedRectangle(bounds.reduced(0.5f), 8.0f, 1.5f);
        }
    };

    RoundedTextEditorLookAndFeel roundedTextEditorLookAndFeel;

    void styleButton(juce::TextButton& button)
    {
        button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.92f));
        button.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    }

    void styleToggleButton(juce::ToggleButton& button)
    {
        button.setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.92f));
        button.setColour(juce::ToggleButton::tickColourId, accentColour());
        button.setColour(juce::ToggleButton::tickDisabledColourId, juce::Colours::white.withAlpha(0.22f));
    }

    void styleEditor(juce::TextEditor& editor)
    {
        editor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        editor.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        editor.setColour(juce::TextEditor::highlightColourId, juce::Colour(0x554a7cff));
        editor.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        editor.setBorder(juce::BorderSize<int>(6));
        editor.setLookAndFeel(&roundedTextEditorLookAndFeel);
    }

    void clearEditorLookAndFeel(juce::TextEditor& editor)
    {
        editor.setLookAndFeel(nullptr);
    }
}

void TirinatorAudioProcessorEditor::SpectralView::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced(1.0f);
    const auto bg = mode == TabMode::velocity ? juce::Colour(0xff233244) : juce::Colour(0xff203546);
    const auto accent = mode == TabMode::velocity ? juce::Colour(0xff7aa9ff) : juce::Colour(0xff82d6c8);
    const auto lightAccent = juce::Colour(0xffb7dcff);
    const auto darkAccent = juce::Colour(0xff2f6fbf);

    g.setColour(bg);
    g.fillRoundedRectangle(area, 8.0f);

    g.setColour(juce::Colour(0xff4f5b6f).withAlpha(0.65f));
    g.drawRoundedRectangle(area, 8.0f, 1.0f);

    auto inner = area.reduced(4.0f, 4.0f);

    if (bands.empty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.setFont(juce::Font(11.0f, juce::Font::plain));
        g.drawText("[no bands]", inner.toNearestInt(), juce::Justification::centred, false);
        return;
    }

    g.setColour(juce::Colour(0xffffffff).withAlpha(0.08f));
    for (int i = 0; i <= 8; ++i)
    {
        const float x = inner.getX() + inner.getWidth() * (float)i / 8.0f;
        g.drawVerticalLine((int)std::round(x), inner.getY(), inner.getBottom());
    }

    for (size_t i = 0; i < bands.size(); ++i)
    {
        const auto& band = bands[i];
        const float x1 = inner.getX() + frequencyToX(band.lowHz, inner.getWidth());
        const float x2 = inner.getX() + frequencyToX(band.highHz, inner.getWidth());
        const float left = juce::jmin(x1, x2);
        const float right = juce::jmax(x1, x2);
        const float w = juce::jmax(2.0f, right - left);

        const float volume = i < bandVolumes.size() ? juce::jlimit(0.0f, 1.0f, bandVolumes[i]) : 0.0f;
        const float hue = (float)i / juce::jmax(1.0f, (float)bands.size());
        const auto fill = lightAccent.interpolatedWith(darkAccent, volume);

        g.setColour(fill.withAlpha(0.20f + 0.22f * volume + 0.06f * hue));
        g.fillRoundedRectangle(juce::Rectangle<float>(left, inner.getY() + 1.0f, w, inner.getHeight() - 2.0f), 5.0f);

        g.setColour(accent.interpolatedWith(darkAccent, volume).withAlpha(0.58f + 0.28f * volume));
        g.drawVerticalLine((int)std::round(left), inner.getY(), inner.getBottom());
        if (i == bands.size() - 1)
            g.drawVerticalLine((int)std::round(right), inner.getY(), inner.getBottom());
    }

    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.drawLine(inner.getX(), inner.getCentreY(), inner.getRight(), inner.getCentreY(), 1.0f);
}

TirinatorAudioProcessor::PatternCategory TirinatorAudioProcessorEditor::categoryForTab(TabMode mode)
{
    return mode == TabMode::stereo ? TirinatorAudioProcessor::PatternCategory::stereo
        : TirinatorAudioProcessor::PatternCategory::velocity;
}

TirinatorAudioProcessorEditor::PatternRow::PatternRow()
{
    title.setJustificationType(juce::Justification::centredLeft);
    title.setFont(juce::Font(14.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.92f));

    onLabel.setText("on velocity", juce::dontSendNotification);
    offLabel.setText("off velocity", juce::dontSendNotification);
    stereoLabel.setText("stereo position", juce::dontSendNotification);

    for (auto* label : { &onLabel, &offLabel, &stereoLabel })
    {
        label->setJustificationType(juce::Justification::centredLeft);
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
    }

    onEditor.setInputRestrictions(0, "0123456789.");
    offEditor.setInputRestrictions(0, "0123456789.");
    stereoEditor.setInputRestrictions(0, "0123456789.");
    onEditor.setJustification(juce::Justification::centredLeft);
    offEditor.setJustification(juce::Justification::centredLeft);
    stereoEditor.setJustification(juce::Justification::centredLeft);
    onEditor.setSelectAllWhenFocused(true);
    offEditor.setSelectAllWhenFocused(true);
    stereoEditor.setSelectAllWhenFocused(true);
    styleEditor(onEditor);
    styleEditor(offEditor);
    styleEditor(stereoEditor);

    copyButton.setButtonText("copy & add below");
    copyButton.setTooltip("copy this pattern and insert it below");
    randomizeButton.setButtonText("randomize");
    randomizeButton.setTooltip("randomize the step pattern");
    precisionButton.setButtonText("quarter");
    precisionButton.setTooltip("cycle timing precision");
    removeButton.setButtonText("remove");
    removeButton.setTooltip("remove this pattern");

    styleButton(copyButton);
    styleButton(randomizeButton);
    styleButton(precisionButton);
    styleButton(removeButton);

    addAndMakeVisible(title);
    addAndMakeVisible(onLabel);
    addAndMakeVisible(offLabel);
    addAndMakeVisible(stereoLabel);
    addAndMakeVisible(onEditor);
    addAndMakeVisible(offEditor);
    addAndMakeVisible(stereoEditor);
    addAndMakeVisible(copyButton);
    addAndMakeVisible(randomizeButton);
    addAndMakeVisible(precisionButton);
    addAndMakeVisible(removeButton);

    for (int i = 0; i < maxSteps; ++i)
    {
        auto button = std::make_unique<juce::ToggleButton>(juce::String(i + 1));
        button->setTooltip("timing step " + juce::String(i + 1));
        button->setClickingTogglesState(true);
        button->setColour(juce::ToggleButton::textColourId, juce::Colours::white.withAlpha(0.85f));
        button->setColour(juce::ToggleButton::tickColourId, accentColour());
        button->setColour(juce::ToggleButton::tickDisabledColourId, juce::Colours::white.withAlpha(0.22f));
        addAndMakeVisible(*button);
        stepButtons[(size_t)i] = std::move(button);
    }

    setRepaintsOnMouseActivity(true);
}

void TirinatorAudioProcessorEditor::PatternRow::setIndex(int newIndex)
{
    index = newIndex;
    title.setText("pattern " + juce::String(index + 1), juce::dontSendNotification);
}

void TirinatorAudioProcessorEditor::PatternRow::setSpec(const PatternSpec& spec)
{
    precisionIndex = juce::jlimit(0, TirinatorAudioProcessor::precisionCount - 1, spec.precisionIndex);
    visibleStepCount = TirinatorAudioProcessor::getStepCountForPrecision(precisionIndex);
    patternMask = spec.patternMask & TirinatorAudioProcessor::maskForStepCount(visibleStepCount);
    onVelocity = juce::jlimit(0.0f, 1.0f, spec.onVelocity);
    offVelocity = juce::jlimit(0.0f, 1.0f, spec.offVelocity);
    stereoPosition = juce::jlimit(0.0f, 1.0f, spec.stereoPosition);

    precisionButton.setButtonText(TirinatorAudioProcessor::getPrecisionName(precisionIndex));
    onEditor.setText(juce::String(onVelocity, 2), juce::dontSendNotification);
    offEditor.setText(juce::String(offVelocity, 2), juce::dontSendNotification);
    stereoEditor.setText(juce::String(stereoPosition, 2), juce::dontSendNotification);

    for (int i = 0; i < maxSteps; ++i)
    {
        auto* stepButton = stepButtons[(size_t)i].get();
        const bool visible = i < visibleStepCount;
        stepButton->setVisible(visible);
        stepButton->setToggleState((patternMask & (1u << i)) != 0, juce::dontSendNotification);
    }

    setMode(mode);
}

void TirinatorAudioProcessorEditor::PatternRow::setMode(TabMode newMode)
{
    mode = newMode;

    const bool showVelocity = (mode == TabMode::velocity);
    const bool showStereo = (mode == TabMode::stereo);

    onLabel.setVisible(showVelocity);
    onEditor.setVisible(showVelocity);
    offLabel.setVisible(showVelocity);
    offEditor.setVisible(showVelocity);

    stereoLabel.setVisible(showStereo);
    stereoEditor.setVisible(showStereo);

    resized();
}

int TirinatorAudioProcessorEditor::PatternRow::getVisibleStepCount() const
{
    return visibleStepCount;
}

uint32_t TirinatorAudioProcessorEditor::PatternRow::getPatternMask() const
{
    uint32_t mask = 0u;

    for (int i = 0; i < visibleStepCount; ++i)
    {
        if (auto* button = stepButtons[(size_t)i].get(); button != nullptr && button->getToggleState())
            mask |= (1u << i);
    }

    return mask;
}

int TirinatorAudioProcessorEditor::PatternRow::getPrecisionIndex() const
{
    return precisionIndex;
}

float TirinatorAudioProcessorEditor::PatternRow::getOnVelocity() const
{
    return onVelocity;
}

float TirinatorAudioProcessorEditor::PatternRow::getOffVelocity() const
{
    return offVelocity;
}

float TirinatorAudioProcessorEditor::PatternRow::getStereoPosition() const
{
    return stereoPosition;
}

juce::ToggleButton* TirinatorAudioProcessorEditor::PatternRow::getStepButton(int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= maxSteps)
        return nullptr;

    return stepButtons[(size_t)stepIndex].get();
}

const juce::ToggleButton* TirinatorAudioProcessorEditor::PatternRow::getStepButton(int stepIndex) const
{
    if (stepIndex < 0 || stepIndex >= maxSteps)
        return nullptr;

    return stepButtons[(size_t)stepIndex].get();
}

void TirinatorAudioProcessorEditor::PatternRow::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced(2.0f);

    g.setColour(juce::Colour(0xff202531));
    g.fillRoundedRectangle(area, 14.0f);

    g.setColour(juce::Colour(0xff4f5b6f).withAlpha(0.75f));
    g.drawRoundedRectangle(area, 14.0f, 1.0f);
}

void TirinatorAudioProcessorEditor::PatternRow::resized()
{
    auto area = getLocalBounds().reduced(10, 8);

    auto top = area.removeFromTop(24);
    top.removeFromRight(4);
    removeButton.setBounds(top.removeFromRight(80));
    top.removeFromRight(8);
    randomizeButton.setBounds(top.removeFromRight(92));
    top.removeFromRight(8);
    precisionButton.setBounds(top.removeFromRight(100));
    top.removeFromRight(8);
    copyButton.setBounds(top.removeFromRight(132));
    top.removeFromRight(8);
    title.setBounds(top.removeFromLeft(160));

    area.removeFromTop(8);

    if (mode == TabMode::velocity)
    {
        auto row = area.removeFromTop(34);
        onLabel.setBounds(row.removeFromLeft(84));
        {
            auto bounds = row.removeFromLeft(68);
            onEditor.setBounds(bounds.getX(), bounds.getY(), bounds.getWidth(), 34);
        }
        row.removeFromLeft(12);
        offLabel.setBounds(row.removeFromLeft(88));
        {
            auto bounds = row.removeFromLeft(68);
            offEditor.setBounds(bounds.getX(), bounds.getY(), bounds.getWidth(), 34);
        }
    }
    else if (mode == TabMode::stereo)
    {
        auto row = area.removeFromTop(34);
        stereoLabel.setBounds(row.removeFromLeft(102));
        {
            auto bounds = row.removeFromLeft(74);
            stereoEditor.setBounds(bounds.getX(), bounds.getY(), bounds.getWidth(), 34);
        }
    }

    area.removeFromTop(6);

    auto strip = area.removeFromTop(24);
    const int visibleCount = juce::jmax(1, visibleStepCount);
    const int gap = 4;
    const int buttonWidth = juce::jmax(16, (strip.getWidth() - gap * (visibleCount - 1)) / visibleCount);

    for (int i = 0; i < maxSteps; ++i)
    {
        auto* button = stepButtons[(size_t)i].get();
        if (button == nullptr)
            continue;

        if (i >= visibleStepCount)
        {
            button->setBounds(0, 0, 0, 0);
            continue;
        }

        const int x = strip.getX() + i * (buttonWidth + gap);
        button->setBounds(x, strip.getY(), buttonWidth, 24);
    }
}

TirinatorAudioProcessorEditor::BandRow::BandRow()
{
    title.setJustificationType(juce::Justification::centredLeft);
    title.setFont(juce::Font(17.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colours::white);

    lowLabel.setText("low", juce::dontSendNotification);
    highLabel.setText("high", juce::dontSendNotification);
    patternLabel.setText("patterns", juce::dontSendNotification);

    for (auto* label : { &lowLabel, &highLabel, &patternLabel })
    {
        label->setJustificationType(juce::Justification::centredLeft);
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.82f));
    }

    lowEditor.setInputRestrictions(0, "0123456789.");
    highEditor.setInputRestrictions(0, "0123456789.");
    lowEditor.setJustification(juce::Justification::centredLeft);
    highEditor.setJustification(juce::Justification::centredLeft);
    lowEditor.setSelectAllWhenFocused(true);
    highEditor.setSelectAllWhenFocused(true);
    styleEditor(lowEditor);
    styleEditor(highEditor);

    addPatternButton.setButtonText("add pattern");
    removeBandButton.setButtonText("remove band");
    styleButton(addPatternButton);
    styleButton(removeBandButton);
    addPatternButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff243244));
    removeBandButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff35242a));

    addAndMakeVisible(title);
    addAndMakeVisible(lowLabel);
    addAndMakeVisible(highLabel);
    addAndMakeVisible(patternLabel);
    addAndMakeVisible(lowEditor);
    addAndMakeVisible(highEditor);
    addAndMakeVisible(addPatternButton);
    addAndMakeVisible(removeBandButton);

    setRepaintsOnMouseActivity(true);
}

void TirinatorAudioProcessorEditor::BandRow::setIndex(int newIndex)
{
    index = newIndex;
    title.setText("band " + juce::String(index + 1), juce::dontSendNotification);
}

void TirinatorAudioProcessorEditor::BandRow::setSpec(const BandSpec& spec)
{
    lowEditor.setText(juce::String(spec.lowHz, 1), juce::dontSendNotification);
    highEditor.setText(juce::String(spec.highHz, 1), juce::dontSendNotification);

    patterns.clear(true);
    for (int i = 0; i < static_cast<int>(spec.patterns.size()); ++i)
    {
        auto* row = new PatternRow();
        row->setIndex(i);
        row->setSpec(spec.patterns[(size_t)i]);
        row->removeButton.setEnabled(true);
        patterns.add(row);
        addAndMakeVisible(row);
    }

    addPatternButton.setEnabled(true);
    resized();
}

void TirinatorAudioProcessorEditor::BandRow::setMode(TabMode newMode)
{
    mode = newMode;
    for (auto* pattern : patterns)
        if (pattern != nullptr)
            pattern->setMode(mode);
}

int TirinatorAudioProcessorEditor::BandRow::getPatternCount() const
{
    return patterns.size();
}

int TirinatorAudioProcessorEditor::BandRow::getPreferredHeight() const
{
    const int patternGap = patterns.isEmpty() ? 0 : (patterns.size() - 1) * 8;
    const int patternArea = patterns.size() * patternRowHeight + patternGap;
    return 26 + 10 + editorRowHeight + 10 + 20 + 6 + patternArea + 12;
}

TirinatorAudioProcessorEditor::PatternRow* TirinatorAudioProcessorEditor::BandRow::getPatternRow(int patternIndex)
{
    if (patternIndex < 0 || patternIndex >= patterns.size())
        return nullptr;

    return patterns[(size_t)patternIndex];
}

const TirinatorAudioProcessorEditor::PatternRow* TirinatorAudioProcessorEditor::BandRow::getPatternRow(int patternIndex) const
{
    if (patternIndex < 0 || patternIndex >= patterns.size())
        return nullptr;

    return patterns[(size_t)patternIndex];
}

void TirinatorAudioProcessorEditor::BandRow::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced(2.0f);

    g.setColour(panelColour());
    g.fillRoundedRectangle(area, 18.0f);

    g.setColour(juce::Colour(0xff4f5b6f).withAlpha(0.75f));
    g.drawRoundedRectangle(area, 18.0f, 1.0f);

    auto header = getLocalBounds().removeFromTop(42).toFloat().reduced(6.0f, 6.0f);
    g.setColour(juce::Colour(0xff283041));
    g.fillRoundedRectangle(header, 12.0f);

    g.setColour(juce::Colour(0xff7aa9ff).withAlpha(0.08f));
    g.fillRoundedRectangle(header.removeFromLeft(4.0f), 12.0f);
}

void TirinatorAudioProcessorEditor::BandRow::resized()
{
    auto area = getLocalBounds().reduced(14, 8);

    auto top = area.removeFromTop(24);
    top.translate(0, 1);
    removeBandButton.setBounds(top.removeFromRight(96));
    top.removeFromRight(8);
    addPatternButton.setBounds(top.removeFromRight(96));
    top.removeFromRight(8);
    title.setBounds(top);

    area.removeFromTop(8);

    auto editors = area.removeFromTop(editorRowHeight);
    editors.translate(0, 3);
    lowLabel.setBounds(editors.removeFromLeft(32));
    lowEditor.setBounds(editors.removeFromLeft(112));
    editors.removeFromLeft(12);
    highLabel.setBounds(editors.removeFromLeft(36));
    highEditor.setBounds(editors.removeFromLeft(112));

    area.removeFromTop(8);

    auto rowLabel = area.removeFromTop(20);
    patternLabel.setBounds(rowLabel.removeFromLeft(120));

    area.removeFromTop(6);

    for (auto* patternRow : patterns)
    {
        patternRow->setBounds(area.removeFromTop(patternRowHeight));
        area.removeFromTop(8);
    }
}

TirinatorAudioProcessorEditor::TirinatorAudioProcessorEditor(TirinatorAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    titleLabel.setText("tirinator", juce::dontSendNotification);
    titleLabel.setJustificationType(juce::Justification::centredLeft);
    titleLabel.setFont(juce::Font(26.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);

    hintLabel.setText("a midi-like multiband splitter", juce::dontSendNotification);
    hintLabel.setJustificationType(juce::Justification::centredLeft);
    hintLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.70f));
    hintLabel.setFont(juce::Font(13.0f));

    noBandsLabel.setText("[no bands]", juce::dontSendNotification);
    noBandsLabel.setJustificationType(juce::Justification::centred);
    noBandsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.55f));
    noBandsLabel.setFont(juce::Font(12.0f));

    bypassButton.setButtonText("bypass");
    velocityTabButton.setButtonText("velocity");
    stereoTabButton.setButtonText("stereo");
    settingsTabButton.setButtonText("settings");
    infoTabButton.setButtonText("info");

    for (auto* button : { &bypassButton, &velocityTabButton, &stereoTabButton, &settingsTabButton, &infoTabButton })
    {
        button->setClickingTogglesState(true);
        styleButton(*button);
        addAndMakeVisible(*button);
    }

    countLabel.setText("bands", juce::dontSendNotification);
    countLabel.setJustificationType(juce::Justification::centredLeft);
    countLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));

    countEditor.setInputRestrictions(4, "0123456789");
    countEditor.setJustification(juce::Justification::centredLeft);
    countEditor.setSelectAllWhenFocused(true);
    styleEditor(countEditor);

    applyCountButton.setButtonText("apply");
    addBandButton.setButtonText("add band");
    removeBandButton.setButtonText("remove last band");

    styleButton(applyCountButton);
    styleButton(addBandButton);
    styleButton(removeBandButton);

    defaultOnLabel.setText("default on velocity", juce::dontSendNotification);
    defaultOffLabel.setText("default off velocity", juce::dontSendNotification);
    for (auto* label : { &defaultOnLabel, &defaultOffLabel })
    {
        label->setJustificationType(juce::Justification::centredLeft);
        label->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));
    }

    defaultOnEditor.setInputRestrictions(0, "0123456789.");
    defaultOffEditor.setInputRestrictions(0, "0123456789.");
    defaultOnEditor.setJustification(juce::Justification::centredLeft);
    defaultOffEditor.setJustification(juce::Justification::centredLeft);
    defaultOnEditor.setSelectAllWhenFocused(true);
    defaultOffEditor.setSelectAllWhenFocused(true);
    styleEditor(defaultOnEditor);
    styleEditor(defaultOffEditor);

    defaultStereoLabel.setText("default stereo position", juce::dontSendNotification);
    defaultStereoLabel.setJustificationType(juce::Justification::centredLeft);
    defaultStereoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.88f));

    defaultStereoEditor.setInputRestrictions(0, "0123456789.");
    defaultStereoEditor.setJustification(juce::Justification::centredLeft);
    defaultStereoEditor.setSelectAllWhenFocused(true);
    styleEditor(defaultStereoEditor);

    spectralViewToggle.setButtonText("show spectral view");
    spectralViewToggle.setClickingTogglesState(true);
    styleToggleButton(spectralViewToggle);

    infoText.setMultiLine(true);
    infoText.setReturnKeyStartsNewLine(true);
    infoText.setReadOnly(true);
    infoText.setCaretVisible(false);
    infoText.setPopupMenuEnabled(false);
    infoText.setText("tirinator is a midi-like multiband splitter featuring the following:\n\n"
        "  velocity tab:\n"
        "    use low/high to define each band\n"
        "    turn steps on and off with the numbered buttons\n"
        "    on velocity / off velocity controls how loud each step is\n\n"
        "  stereo tab:\n"
        "    use stereo position to pan the active steps\n"
        "    the step pattern still decides when the band plays\n\n"
        "  settings tab:\n"
        "    set the default on/off velocities and stereo position\n"
        "    use the toggle at the bottom to hide the spectral view\n\n"
        "  bypass:\n"
        "    bypasses the plugin (incredible)\n\n"
        "  tips:\n"
        "    general:\n"
        "      the design of tirinator is hierarchal; modes own bands, bands own patterns, patterns own properties & steps, and steps own properties.\n"
        "    for power usage:\n"
        "      click 'copy & add below' to  ... well, i think you can guess ...\n"
        "      click 'precision' to change how many steps each pattern uses\n"
        "      click 'randomize' to quickly get a (possibly good) base going\n\n\n\n"
        "v0.9.0, made with JUCE <3");
    styleEditor(infoText);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(hintLabel);
    addAndMakeVisible(countLabel);
    addAndMakeVisible(countEditor);
    addAndMakeVisible(applyCountButton);
    addAndMakeVisible(addBandButton);
    addAndMakeVisible(removeBandButton);
    addAndMakeVisible(defaultOnLabel);
    addAndMakeVisible(defaultOffLabel);
    addAndMakeVisible(defaultStereoLabel);
    addAndMakeVisible(defaultOnEditor);
    addAndMakeVisible(defaultOffEditor);
    addAndMakeVisible(defaultStereoEditor);
    addAndMakeVisible(spectralViewToggle);
    addAndMakeVisible(infoText);

    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&rowsContainer, false);
    viewport.setScrollBarsShown(true, false);

    addAndMakeVisible(spectralView);
    spectralView.setInterceptsMouseClicks(false, false);

    addAndMakeVisible(noBandsLabel);
    noBandsLabel.toFront(false);

    bypassButton.addListener(this);
    velocityTabButton.addListener(this);
    stereoTabButton.addListener(this);
    settingsTabButton.addListener(this);
    infoTabButton.addListener(this);
    applyCountButton.addListener(this);
    addBandButton.addListener(this);
    removeBandButton.addListener(this);
    countEditor.addListener(this);
    defaultOnEditor.addListener(this);
    defaultOffEditor.addListener(this);
    defaultStereoEditor.addListener(this);
    spectralViewToggle.addListener(this);

    activeTab = TabMode::velocity;
    bypassButton.setToggleState(audioProcessor.isBypassed(), juce::dontSendNotification);
    syncTabButtons();

    setSize(1180, 760);
    refreshFromProcessor();
    startTimerHz(60);
}
TirinatorAudioProcessorEditor::~TirinatorAudioProcessorEditor()
{
    stopTimer();

    clearEditorLookAndFeel(countEditor);
    clearEditorLookAndFeel(defaultOnEditor);
    clearEditorLookAndFeel(defaultOffEditor);
    clearEditorLookAndFeel(defaultStereoEditor);
    clearEditorLookAndFeel(infoText);

    for (auto* row : rows)
    {
        if (row != nullptr)
        {
            clearEditorLookAndFeel(row->lowEditor);
            clearEditorLookAndFeel(row->highEditor);

            for (int i = 0; i < row->getPatternCount(); ++i)
            {
                if (auto* patternRow = row->getPatternRow(i); patternRow != nullptr)
                {
                    clearEditorLookAndFeel(patternRow->onEditor);
                    clearEditorLookAndFeel(patternRow->offEditor);
                    clearEditorLookAndFeel(patternRow->stereoEditor);
                }
            }
        }
    }

    bypassButton.removeListener(this);
    velocityTabButton.removeListener(this);
    stereoTabButton.removeListener(this);
    settingsTabButton.removeListener(this);
    infoTabButton.removeListener(this);
    applyCountButton.removeListener(this);
    addBandButton.removeListener(this);
    removeBandButton.removeListener(this);
    countEditor.removeListener(this);
    defaultOnEditor.removeListener(this);
    defaultOffEditor.removeListener(this);
    defaultStereoEditor.removeListener(this);
    spectralViewToggle.removeListener(this);
}
void TirinatorAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour());

    auto bounds = getLocalBounds().reduced(8);
    g.setColour(juce::Colour(0xff171b22));
    g.fillRoundedRectangle(bounds.toFloat(), 22.0f);

    g.setColour(juce::Colour(0xff5d6a80).withAlpha(0.30f));
    g.drawRoundedRectangle(bounds.toFloat(), 22.0f, 1.0f);
}

void TirinatorAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(sidePad, topPad);
    auto header = area;
    int headerUsed = 0;

    auto topRow = header.removeFromTop(30);
    headerUsed += 30;
    titleLabel.setBounds(topRow.removeFromLeft(260));

    auto tabRow = topRow.removeFromRight(452);
    bypassButton.setBounds(tabRow.removeFromLeft(72));
    tabRow.removeFromLeft(8);
    velocityTabButton.setBounds(tabRow.removeFromLeft(92));
    tabRow.removeFromLeft(8);
    stereoTabButton.setBounds(tabRow.removeFromLeft(92));
    tabRow.removeFromLeft(8);
    settingsTabButton.setBounds(tabRow.removeFromLeft(92));
    tabRow.removeFromLeft(8);
    infoTabButton.setBounds(tabRow.removeFromLeft(72));

    header.removeFromTop(8);
    headerUsed += 8;
    hintLabel.setBounds(header.removeFromTop(22));
    headerUsed += 22;

    header.removeFromTop(16);
    headerUsed += 16;

    if (activeTab == TabMode::settings)
    {
        spectralView.setVisible(false);
        infoText.setVisible(false);
        auto settingsRow = header.removeFromTop(34);
        defaultOnLabel.setBounds(settingsRow.removeFromLeft(180));
        defaultOnEditor.setBounds(settingsRow.removeFromLeft(72));
        headerUsed += 34;

        header.removeFromTop(8);
        headerUsed += 8;

        auto settingsRow2 = header.removeFromTop(34);
        defaultOffLabel.setBounds(settingsRow2.removeFromLeft(180));
        defaultOffEditor.setBounds(settingsRow2.removeFromLeft(72));
        headerUsed += 34;

        header.removeFromTop(8);
        headerUsed += 8;

        auto settingsRow3 = header.removeFromTop(34);
        defaultStereoLabel.setBounds(settingsRow3.removeFromLeft(180));
        defaultStereoEditor.setBounds(settingsRow3.removeFromLeft(72));
        headerUsed += 34;

        header.removeFromTop(8);
        headerUsed += 8;

        auto settingsRow4 = header.removeFromTop(34);
        spectralViewToggle.setBounds(settingsRow4);
        headerUsed += 34;

        countLabel.setVisible(false);
        countEditor.setVisible(false);
        applyCountButton.setVisible(false);
        addBandButton.setVisible(false);
        removeBandButton.setVisible(false);
        viewport.setVisible(false);
        noBandsLabel.setVisible(false);
        defaultOnLabel.setVisible(true);
        defaultOnEditor.setVisible(true);
        defaultOffLabel.setVisible(true);
        defaultOffEditor.setVisible(true);
        defaultStereoLabel.setVisible(true);
        defaultStereoEditor.setVisible(true);
        spectralViewToggle.setVisible(true);
    }
    else if (activeTab == TabMode::info)
    {
        spectralView.setVisible(false);
        infoText.setVisible(true);
        defaultOnLabel.setVisible(false);
        defaultOnEditor.setVisible(false);
        defaultOffLabel.setVisible(false);
        defaultOffEditor.setVisible(false);
        defaultStereoLabel.setVisible(false);
        defaultStereoEditor.setVisible(false);
        spectralViewToggle.setVisible(false);
        noBandsLabel.setVisible(false);
        countLabel.setVisible(false);
        countEditor.setVisible(false);
        applyCountButton.setVisible(false);
        addBandButton.setVisible(false);
        removeBandButton.setVisible(false);
        viewport.setVisible(false);
    }
    else
    {
        spectralView.setVisible(audioProcessor.isSpectralViewEnabled());
        infoText.setVisible(false);
        defaultOnLabel.setVisible(false);
        defaultOnEditor.setVisible(false);
        defaultOffLabel.setVisible(false);
        defaultOffEditor.setVisible(false);
        defaultStereoLabel.setVisible(false);
        defaultStereoEditor.setVisible(false);
        spectralViewToggle.setVisible(false);
        noBandsLabel.setVisible(false);

        countLabel.setVisible(true);
        countEditor.setVisible(true);
        applyCountButton.setVisible(true);
        addBandButton.setVisible(true);
        removeBandButton.setVisible(true);
        viewport.setVisible(true);

        auto controls = header.removeFromTop(34);
        countLabel.setBounds(controls.removeFromLeft(50));
        countEditor.setBounds(controls.removeFromLeft(70));
        controls.removeFromLeft(8);
        applyCountButton.setBounds(controls.removeFromLeft(72));
        controls.removeFromLeft(8);
        addBandButton.setBounds(controls.removeFromLeft(86));
        controls.removeFromLeft(8);
        removeBandButton.setBounds(controls.removeFromLeft(120));
        controls.removeFromLeft(8);
        if (audioProcessor.isSpectralViewEnabled())
        {
            const int spectralWidth = juce::jmin(220, juce::jmax(120, controls.getWidth()));
            spectralView.setBounds(controls.removeFromLeft(spectralWidth));
        }
        else
        {
            spectralView.setBounds(0, 0, 0, 0);
        }
        headerUsed += 34;

        header.removeFromTop(8);
        headerUsed += 8;
    }

    header.removeFromTop(16);
    headerUsed += 16;

    area.removeFromTop(headerUsed);

    if (activeTab == TabMode::info)
    {
        infoText.setBounds(area.reduced(0, 0));
        return;
    }

    viewport.setBounds(area);
    layoutRows();
}

void TirinatorAudioProcessorEditor::layoutRows()
{
    int y = 0;
    const int contentWidth = juce::jmax(viewport.getWidth(), 1) - viewport.getScrollBarThickness();

    for (auto* row : rows)
    {
        const int rowHeight = row->getPreferredHeight();
        row->setBounds(0, y, contentWidth, rowHeight);
        y += rowHeight + bandGap;
    }

    rowsContainer.setSize(juce::jmax(viewport.getWidth(), 1), juce::jmax(y, 1));

    const bool showNoBands = (activeTab != TabMode::settings && rows.isEmpty());
    noBandsLabel.setVisible(showNoBands);
    if (showNoBands)
    {
        auto bounds = viewport.getBounds();
        noBandsLabel.setBounds(bounds.getX(), bounds.getCentreY() - 10, bounds.getWidth(), 20);
    }
}

void TirinatorAudioProcessorEditor::syncTabButtons()
{
    bypassButton.setToggleState(audioProcessor.isBypassed(), juce::dontSendNotification);
    velocityTabButton.setToggleState(activeTab == TabMode::velocity, juce::dontSendNotification);
    stereoTabButton.setToggleState(activeTab == TabMode::stereo, juce::dontSendNotification);
    settingsTabButton.setToggleState(activeTab == TabMode::settings, juce::dontSendNotification);
    infoTabButton.setToggleState(activeTab == TabMode::info, juce::dontSendNotification);
    spectralViewToggle.setToggleState(audioProcessor.isSpectralViewEnabled(), juce::dontSendNotification);

    bypassButton.setColour(juce::TextButton::buttonColourId,
        bypassButton.getToggleState() ? juce::Colour(0xff384f42)
        : juce::Colour(0xff232833));

    velocityTabButton.setColour(juce::TextButton::buttonColourId,
        activeTab == TabMode::velocity ? accentColour()
        : juce::Colour(0xff232833));
    stereoTabButton.setColour(juce::TextButton::buttonColourId,
        activeTab == TabMode::stereo ? accentColour()
        : juce::Colour(0xff232833));
    settingsTabButton.setColour(juce::TextButton::buttonColourId,
        activeTab == TabMode::settings ? accentColour()
        : juce::Colour(0xff232833));
    infoTabButton.setColour(juce::TextButton::buttonColourId,
        activeTab == TabMode::info ? accentColour()
        : juce::Colour(0xff232833));
}

void TirinatorAudioProcessorEditor::setActiveTab(TabMode newMode)
{
    if (activeTab == newMode)
        return;

    activeTab = newMode;
    syncTabButtons();
    refreshFromProcessor();
}

void TirinatorAudioProcessorEditor::updateRowModes()
{
    for (auto* row : rows)
    {
        if (row != nullptr)
            row->setMode(activeTab);
    }
}

void TirinatorAudioProcessorEditor::scheduleRefresh()
{
    juce::Component::SafePointer<TirinatorAudioProcessorEditor> safeThis(this);
    juce::MessageManager::callAsync([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->refreshFromProcessor();
        });
}

void TirinatorAudioProcessorEditor::buttonClicked(juce::Button* button)
{
    if (button == &bypassButton)
    {
        audioProcessor.setBypassed(bypassButton.getToggleState());
        syncTabButtons();
        return;
    }

    if (button == &velocityTabButton)
    {
        setActiveTab(TabMode::velocity);
        return;
    }

    if (button == &stereoTabButton)
    {
        setActiveTab(TabMode::stereo);
        return;
    }

    if (button == &settingsTabButton)
    {
        setActiveTab(TabMode::settings);
        return;
    }

    if (button == &infoTabButton)
    {
        setActiveTab(TabMode::info);
        return;
    }

    if (button == &spectralViewToggle)
    {
        audioProcessor.setSpectralViewEnabled(spectralViewToggle.getToggleState());
        scheduleRefresh();
        return;
    }

    if (button == &applyCountButton)
    {
        updateBandCountFromEditor();
        return;
    }

    if (button == &addBandButton)
    {
        const auto category = categoryForTab(activeTab);
        audioProcessor.setBandCount(category, audioProcessor.getBandCount(category) + 1);
        scheduleRefresh();
        return;
    }

    if (button == &removeBandButton)
    {
        const auto category = categoryForTab(activeTab);
        audioProcessor.setBandCount(category, juce::jmax(0, audioProcessor.getBandCount(category) - 1));
        scheduleRefresh();
        return;
    }

    if (activeTab == TabMode::settings || activeTab == TabMode::info)
        return;

    const auto category = categoryForTab(activeTab);

    for (int bandIndex = 0; bandIndex < rows.size(); ++bandIndex)
    {
        auto* row = rows[(size_t)bandIndex];

        if (button == &row->removeBandButton)
        {
            audioProcessor.removeBand(category, bandIndex);
            scheduleRefresh();
            return;
        }

        if (button == &row->addPatternButton)
        {
            audioProcessor.addBandPattern(category, bandIndex);
            scheduleRefresh();
            return;
        }

        for (int patternIndex = 0; patternIndex < row->getPatternCount(); ++patternIndex)
        {
            auto* patternRow = row->getPatternRow(patternIndex);
            if (patternRow == nullptr)
                continue;

            if (button == &patternRow->randomizeButton)
            {
                const auto spec = audioProcessor.getPatternSpec(category, bandIndex, patternIndex);
                const int stepCount = TirinatorAudioProcessor::getStepCountForPrecision(spec.precisionIndex);
                uint32_t mask = 0u;
                juce::Random rng;

                for (int step = 0; step < stepCount; ++step)
                {
                    if (rng.nextFloat() < 0.5f)
                        mask |= (1u << step);
                }

                if (mask == 0u)
                    mask = (1u << rng.nextInt(juce::jmax(1, stepCount)));

                audioProcessor.setBandPatternSpec(category, bandIndex, patternIndex,
                    spec.precisionIndex, mask,
                    spec.onVelocity, spec.offVelocity, spec.stereoPosition);
                scheduleRefresh();
                return;
            }

            if (button == &patternRow->removeButton)
            {
                audioProcessor.removeBandPattern(category, bandIndex, patternIndex);
                scheduleRefresh();
                return;
            }

            if (button == &patternRow->copyButton)
            {
                audioProcessor.copyAndAddBelowBandPattern(category, bandIndex, patternIndex);
                scheduleRefresh();
                return;
            }

            if (button == &patternRow->precisionButton)
            {
                cyclePatternPrecision(bandIndex, patternIndex);
                return;
            }

            for (int step = 0; step < patternRow->getVisibleStepCount(); ++step)
            {
                if (button == patternRow->getStepButton(step))
                {
                    applyPatternToProcessor(bandIndex, patternIndex);
                    return;
                }
            }
        }
    }
}

void TirinatorAudioProcessorEditor::textEditorTextChanged(juce::TextEditor&)
{
}

void TirinatorAudioProcessorEditor::textEditorFocusLost(juce::TextEditor& editor)
{
    if (&editor == &countEditor)
    {
        juce::Component::SafePointer<TirinatorAudioProcessorEditor> safeThis(this);
        juce::MessageManager::callAsync([safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->updateBandCountFromEditor();
            });
        return;
    }

    if (&editor == &defaultOnEditor || &editor == &defaultOffEditor || &editor == &defaultStereoEditor)
    {
        juce::Component::SafePointer<TirinatorAudioProcessorEditor> safeThis(this);
        juce::MessageManager::callAsync([safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->applyDefaultVelocities();
            });
        return;
    }

    if (activeTab == TabMode::settings || activeTab == TabMode::info)
        return;

    for (int i = 0; i < rows.size(); ++i)
    {
        auto* row = rows[(size_t)i];
        if (&editor == &row->lowEditor || &editor == &row->highEditor)
        {
            juce::Component::SafePointer<TirinatorAudioProcessorEditor> safeThis(this);
            juce::MessageManager::callAsync([safeThis, i]
                {
                    if (safeThis != nullptr)
                        safeThis->applyBandToProcessor(i);
                });
            return;
        }

        for (int p = 0; p < row->getPatternCount(); ++p)
        {
            if (auto* patternRow = row->getPatternRow(p); patternRow != nullptr)
            {
                if (&editor == &patternRow->onEditor
                    || &editor == &patternRow->offEditor
                    || &editor == &patternRow->stereoEditor)
                {
                    juce::Component::SafePointer<TirinatorAudioProcessorEditor> safeThis(this);
                    juce::MessageManager::callAsync([safeThis, i, p]
                        {
                            if (safeThis != nullptr)
                                safeThis->applyPatternToProcessor(i, p);
                        });
                    return;
                }
            }
        }
    }
}

void TirinatorAudioProcessorEditor::timerCallback()
{
    if (juce::ModifierKeys::getCurrentModifiersRealtime().isAnyMouseButtonDown())
        return;

    updateSpectralViewActivity();

    bool anyEditorFocused = countEditor.hasKeyboardFocus(true)
        || defaultOnEditor.hasKeyboardFocus(true)
        || defaultOffEditor.hasKeyboardFocus(true)
        || defaultStereoEditor.hasKeyboardFocus(true)
        || infoText.hasKeyboardFocus(true)
        || bypassButton.hasKeyboardFocus(true)
        || velocityTabButton.hasKeyboardFocus(true)
        || stereoTabButton.hasKeyboardFocus(true)
        || settingsTabButton.hasKeyboardFocus(true)
        || infoTabButton.hasKeyboardFocus(true)
        || applyCountButton.hasKeyboardFocus(true)
        || addBandButton.hasKeyboardFocus(true)
        || removeBandButton.hasKeyboardFocus(true);

    if (!anyEditorFocused)
    {
        for (auto* row : rows)
        {
            if (row == nullptr)
                continue;

            if (row->lowEditor.hasKeyboardFocus(true) || row->highEditor.hasKeyboardFocus(true))
            {
                anyEditorFocused = true;
                break;
            }

            for (int i = 0; i < row->getPatternCount(); ++i)
            {
                if (auto* patternRow = row->getPatternRow(i); patternRow != nullptr)
                {
                    if (patternRow->onEditor.hasKeyboardFocus(true)
                        || patternRow->offEditor.hasKeyboardFocus(true)
                        || patternRow->stereoEditor.hasKeyboardFocus(true)
                        || patternRow->copyButton.hasKeyboardFocus(true)
                        || patternRow->precisionButton.hasKeyboardFocus(true)
                        || patternRow->removeButton.hasKeyboardFocus(true)
                        || patternRow->randomizeButton.hasKeyboardFocus(true))
                    {
                        anyEditorFocused = true;
                        break;
                    }

                    for (int step = 0; step < patternRow->getVisibleStepCount(); ++step)
                    {
                        if (auto* b = patternRow->getStepButton(step); b != nullptr && b->hasKeyboardFocus(true))
                        {
                            anyEditorFocused = true;
                            break;
                        }
                    }
                }

                if (anyEditorFocused)
                    break;
            }

            if (anyEditorFocused)
                break;
        }
    }

    if (anyEditorFocused)
        return;

    refreshFromProcessor();
}

void TirinatorAudioProcessorEditor::updateSpectralViewActivity()
{
    if (activeTab == TabMode::settings || activeTab == TabMode::info || !audioProcessor.isSpectralViewEnabled() || !spectralView.isVisible())
        return;

    const auto category = categoryForTab(activeTab);
    const auto bands = audioProcessor.getBandSpecs(category);

    std::vector<float> bandVolumes;
    bandVolumes.reserve(bands.size());

    const double beatPosition = audioProcessor.getPlaybackBeatPosition();
    for (const auto& band : bands)
        bandVolumes.push_back(bandGainAtBeat(band, beatPosition));

    spectralView.setBandVolumes(std::move(bandVolumes));
}

void TirinatorAudioProcessorEditor::refreshFromProcessor()
{
    refreshing = true;

    syncTabButtons();
    defaultOnEditor.setText(juce::String(audioProcessor.getDefaultOnVelocity(), 2), juce::dontSendNotification);
    defaultOffEditor.setText(juce::String(audioProcessor.getDefaultOffVelocity(), 2), juce::dontSendNotification);
    defaultStereoEditor.setText(juce::String(audioProcessor.getDefaultStereoPosition(), 2), juce::dontSendNotification);

    if (activeTab == TabMode::settings)
    {
        rowsContainer.removeAllChildren();
        rows.clear(true);
        viewport.setVisible(false);
        spectralView.setVisible(false);
        infoText.setVisible(false);
    }
    else if (activeTab == TabMode::info)
    {
        rowsContainer.removeAllChildren();
        rows.clear(true);
        viewport.setVisible(false);
        spectralView.setVisible(false);
        infoText.setVisible(true);
    }
    else
    {
        viewport.setVisible(true);
        spectralView.setVisible(audioProcessor.isSpectralViewEnabled());
        infoText.setVisible(false);
        const auto category = categoryForTab(activeTab);
        countEditor.setText(juce::String(audioProcessor.getBandCount(category)), juce::dontSendNotification);
        const auto bands = audioProcessor.getBandSpecs(category);
        spectralView.setMode(activeTab);
        spectralView.setBands(bands);
        updateSpectralViewActivity();

        rebuildRows();
        layoutRows();
    }

    resized();
    refreshing = false;
}

void TirinatorAudioProcessorEditor::rebuildRows()
{
    rowsContainer.removeAllChildren();
    rows.clear(true);

    if (activeTab == TabMode::settings || activeTab == TabMode::info)
        return;

    const auto category = categoryForTab(activeTab);
    const int count = audioProcessor.getBandCount(category);
    rows.ensureStorageAllocated(count);

    for (int i = 0; i < count; ++i)
    {
        auto* row = new BandRow();
        row->setIndex(i);
        row->setSpec(audioProcessor.getBandSpec(category, i));
        row->setMode(activeTab);

        row->lowEditor.addListener(this);
        row->highEditor.addListener(this);
        row->addPatternButton.addListener(this);
        row->removeBandButton.addListener(this);

        for (int p = 0; p < row->getPatternCount(); ++p)
        {
            if (auto* patternRow = row->getPatternRow(p); patternRow != nullptr)
            {
                patternRow->onEditor.addListener(this);
                patternRow->offEditor.addListener(this);
                patternRow->stereoEditor.addListener(this);
                patternRow->copyButton.addListener(this);
                patternRow->randomizeButton.addListener(this);
                patternRow->precisionButton.addListener(this);
                patternRow->removeButton.addListener(this);
                for (int step = 0; step < PatternRow::maxSteps; ++step)
                    if (auto* b = patternRow->getStepButton(step); b != nullptr)
                        b->addListener(this);
            }
        }

        rows.add(row);
        rowsContainer.addAndMakeVisible(row);
    }

    updateRowModes();
}

void TirinatorAudioProcessorEditor::applyBandToProcessor(int index)
{
    if (refreshing || activeTab == TabMode::settings || activeTab == TabMode::info)
        return;

    const auto category = categoryForTab(activeTab);
    if (index < 0 || index >= rows.size())
        return;

    const auto* row = rows[(size_t)index];
    const auto current = audioProcessor.getBandSpec(category, index);

    const float low = parseFloat(row->lowEditor.getText(), current.lowHz);
    const float high = parseFloat(row->highEditor.getText(), current.highHz);

    audioProcessor.setBandSpec(category, index, low, high);
    scheduleRefresh();
}

void TirinatorAudioProcessorEditor::updateBandCountFromEditor()
{
    if (refreshing || activeTab == TabMode::settings || activeTab == TabMode::info)
        return;

    const auto category = categoryForTab(activeTab);
    const int desired = juce::jlimit(0, TirinatorAudioProcessor::maxUniqueBands,
        parseInt(countEditor.getText(), audioProcessor.getBandCount(category)));

    if (desired != audioProcessor.getBandCount(category))
    {
        audioProcessor.setBandCount(category, desired);
        scheduleRefresh();
    }
}

void TirinatorAudioProcessorEditor::applyPatternToProcessor(int bandIndex, int patternIndex)
{
    if (refreshing || activeTab == TabMode::settings || activeTab == TabMode::info || bandIndex < 0 || bandIndex >= rows.size())
        return;

    const auto category = categoryForTab(activeTab);
    auto* row = rows[(size_t)bandIndex];
    auto* patternRow = row->getPatternRow(patternIndex);
    if (patternRow == nullptr)
        return;

    const auto current = audioProcessor.getPatternSpec(category, bandIndex, patternIndex);

    float onVelocity = current.onVelocity;
    float offVelocity = current.offVelocity;
    float stereoPosition = current.stereoPosition;

    if (activeTab == TabMode::velocity)
    {
        onVelocity = parseFloat(patternRow->onEditor.getText(), patternRow->getOnVelocity());
        offVelocity = parseFloat(patternRow->offEditor.getText(), patternRow->getOffVelocity());
    }
    else
    {
        stereoPosition = parseFloat(patternRow->stereoEditor.getText(), patternRow->getStereoPosition());
    }

    audioProcessor.setBandPatternSpec(category, bandIndex, patternIndex,
        patternRow->getPrecisionIndex(),
        patternRow->getPatternMask(),
        onVelocity,
        offVelocity,
        stereoPosition);

    scheduleRefresh();
}

void TirinatorAudioProcessorEditor::applyDefaultVelocities()
{
    const float onValue = parseFloat(defaultOnEditor.getText(), audioProcessor.getDefaultOnVelocity());
    const float offValue = parseFloat(defaultOffEditor.getText(), audioProcessor.getDefaultOffVelocity());
    const float stereoValue = parseFloat(defaultStereoEditor.getText(), audioProcessor.getDefaultStereoPosition());
    audioProcessor.setDefaultOnVelocity(onValue);
    audioProcessor.setDefaultOffVelocity(offValue);
    audioProcessor.setDefaultStereoPosition(stereoValue);
}

void TirinatorAudioProcessorEditor::cyclePatternPrecision(int bandIndex, int patternIndex)
{
    if (refreshing || activeTab == TabMode::settings || activeTab == TabMode::info || bandIndex < 0 || bandIndex >= rows.size())
        return;

    const auto category = categoryForTab(activeTab);
    const auto spec = audioProcessor.getPatternSpec(category, bandIndex, patternIndex);
    const int nextPrecision = (spec.precisionIndex + 1) % TirinatorAudioProcessor::precisionCount;

    audioProcessor.setBandPatternSpec(category, bandIndex, patternIndex,
        nextPrecision,
        spec.patternMask,
        spec.onVelocity,
        spec.offVelocity,
        spec.stereoPosition);

    scheduleRefresh();
}

float TirinatorAudioProcessorEditor::parseFloat(const juce::String& text, float fallback)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return fallback;

    const auto v = trimmed.getFloatValue();
    if (std::isfinite(v))
        return v;

    return fallback;
}

int TirinatorAudioProcessorEditor::parseInt(const juce::String& text, int fallback)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty())
        return fallback;

    const auto v = trimmed.getIntValue();
    if (v >= 0)
        return v;

    return fallback;
}