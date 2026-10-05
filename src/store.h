// SPDX-License-Identifier: GPL-3.0-or-later

// Files in /mnt/onboard/.adds/nicklegpt: config.ini (model, reasoning effort,
// web search, tag colours, debug logging), books.json (the reader's
// corrections to the book info, per book), and the series lookup in Nickel's
// database.

#ifndef NICKLEGPT_STORE_H
#define NICKLEGPT_STORE_H

#include <QString>

#include "context.h"

#define NGPT_DIR "/mnt/onboard/.adds/nicklegpt"

struct Config {
    QString model;  // default gpt-6.1-sol
    QString effort; // reasoning effort; empty: not sent
    bool    webSearch = true;
    // [tags]: how [[char:Name]] etc. are drawn. style: color, bold, plain or
    // test (color plus a swatch block); colours are #RRGGBB.
    QString tagStyle;
    QString tagChar, tagPlace, tagGroup, tagTerm;
    bool    logAnswers = false; // [debug] log_answers: raw answers in log.txt
};

// Reads config.ini, writing a commented default file first if there is none.
Config load_config();

// The reader's correction of the detected book info. Series, number and title
// stay until reset. Chapter and progress only hold while Nickel still reports
// the chapter that was showing when they were saved: they describe a position,
// and turning to the next chapter makes them stale.
struct BookEdit {
    QString series, number, title, chapter;
    int     percent = -1;
    QString savedAtChapter; // detected chapter when saved
};

bool load_book_edit(QString const &bookKey, BookEdit *edit);
void save_book_edit(QString const &bookKey, BookEdit const &edit);
void clear_book_edit(QString const &bookKey);

// The detected position with the reader's correction applied (fields that
// were changed are listed in `edited`, for the UI).
ReadingContext apply_book_edit(ReadingContext const &detected, BookEdit const &edit, QStringList *edited);

// Series name and number from KoboReader.sqlite (Content::getDbValues doesn't
// have them for the open book). Cached per content id.
void lookup_series(QString const &contentId, QString *series, QString *number);

// Writes a file atomically: tmp file, fsync, rename. Returns false on error.
bool write_file_atomic(QString const &path, QByteArray const &data);

#endif
