// SPDX-License-Identifier: GPL-3.0-or-later

// What the request says about the reader's position: series, book, chapter,
// progress, and the selected text if the request came from a selection.

#ifndef NICKELFOOTNOTE_CONTEXT_H
#define NICKELFOOTNOTE_CONTEXT_H

#include <QString>

class QWidget;

struct ReadingContext {
    bool    found = false; // an open book was found
    QString contentId;
    QString title;
    QString author;
    QString series;
    QString seriesNumber;
    QString chapter;
    int     percent = -1; // in this book; -1 if unknown
    QString selection;

    // One-line summary shown to the reader before sending, for example
    // "The Wheel of Time #3 · The Dragon Reborn · Chapter 20 · 35%".
    QString summary() const;
};

// The ReadingView of the open book, or null.
QWidget *find_reading_view();

// Reads the open book's position. With selection = true, also reads the text
// currently selected in the book.
ReadingContext read_context(bool selection);

#endif
