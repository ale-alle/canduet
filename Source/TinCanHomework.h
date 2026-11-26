// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2026 Tin Can Studios

#pragma once

#include "JuceHeader.h"
#include "TinCanIcons.h"
#include "TinCanSummaries.h"

#include <memory>

// Homework & Recordings (T-090, owner 2026-11-25): one panel for what's reviewed together after a lesson.
// It opens from the folder icon next to Record and docks at the right of the lesson window, where
// Score and Board go (or pops out). Two sections, both icons always shown, the open one bright red:
//   Homework    the site's practice plans (<site>/homework, a web page with the app's sign-in): the
//               lesson's teacher adds and edits the student's homework, everyone else sees their own
//   Recordings  this computer's lesson recordings and highlights (what was Tools > Summaries)
namespace TinCan {

class HomeworkPanel : public Component
{
public:
    // lessonStartMs: the lesson's start (0 in Practice mode), so the page finds the lesson exactly
    HomeworkPanel(SummaryHooks summaryHooks, int64 lessonStartMs);
    ~HomeworkPanel() override;

    void showHomework();
    void showRecordings(const File & select = {});
    void refreshRecordings();

    void paint(Graphics & g) override;
    void resized() override;

private:
    class Browser;
    void showSection(int index);

    IconPicker sections;
    std::unique_ptr<Browser> browser;   // null when WebView2 can't start
    Label noBrowser;
    std::unique_ptr<SummariesPanel> recordings;
    String homeworkUrl;
    bool loaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HomeworkPanel)
};

}
