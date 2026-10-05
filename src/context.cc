// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <NickelHook.h>

#include "context.h"
#include "log.h"
#include "nickel.h"
#include "store.h"

QString ReadingContext::summary() const {
    QStringList parts;
    if (!series.isEmpty())
        parts << (seriesNumber.isEmpty() ? series : series + " #" + seriesNumber);
    if (!title.isEmpty())
        parts << title;
    if (!chapter.isEmpty())
        parts << chapter;
    if (percent >= 0)
        parts << QString::number(percent) + "%";
    return parts.join(QString::fromUtf8(" · "));
}

QWidget *find_reading_view() {
    QWidget *hidden = nullptr;
    for (QWidget *w : QApplication::allWidgets()) {
        if (!w->inherits("ReadingView"))
            continue;
        if (w->isVisible())
            return w;
        if (!hidden)
            hidden = w;
    }
    return hidden;
}

// The book text is drawn by a WebkitView (KepubBookReader for kepubs) inside
// the ReadingView. Other formats (PDF, Adobe EPUB) don't have one.
static QWidget *find_webkit_view(QWidget *rv) {
    for (QWidget *w : rv->findChildren<QWidget*>())
        if (w->inherits("WebkitView") && w->isVisible())
            return w;
    return nullptr;
}

static int db_log_budget = 1; // dump the volume's keys once, to find extra fields

ReadingContext read_context(bool selection) {
    ReadingContext ctx;
    QWidget *rv = find_reading_view();
    if (!rv) {
        ngpt_log("context: no ReadingView");
        return ctx;
    }

    Volume const *vol = ReadingView_getVolume(rv);
    if (!vol || !reinterpret_cast<void* const*>(vol)[1]) { // null d: no book
        ngpt_log("context: ReadingView has no volume");
        return ctx;
    }
    ctx.found = true;

    if (Content_getId) {
        QString id;
        Content_getId(&id, vol);
        ctx.contentId = id;
    }

    {
        alignas(QVariantMap) char buf[sizeof(QVariantMap)];
        QVariantMap *m = reinterpret_cast<QVariantMap*>(buf);
        Content_getDbValues(m, vol);
        ctx.title        = m->value(*Nickel_ATTRIBUTE_TITLE).toString().trimmed();
        ctx.author       = m->value(*Nickel_ATTRIBUTE_ATTRIBUTION).toString().trimmed();
        ctx.series       = m->value(*Nickel_ATTRIBUTE_SERIES).toString().trimmed();
        ctx.seriesNumber = m->value(*Nickel_ATTRIBUTE_SERIES_NUMBER).toString().trimmed();
        if (db_log_budget > 0) {
            db_log_budget--;
            for (auto it = m->constBegin(); it != m->constEnd(); ++it)
                ngpt_log("context: db %s = %s", qPrintable(it.key()), qPrintable(it.value().toString().left(80)));
        }
        m->~QVariantMap();
    }

    if (ctx.series.isEmpty()) // the caller's guard covers this
        lookup_series(ctx.contentId, &ctx.series, &ctx.seriesNumber);

    if (ReadingView_getChapterTitle) {
        QString chapter;
        ReadingView_getChapterTitle(&chapter, rv);
        ctx.chapter = chapter.simplified();
    }
    if (ReadingView_getCalculatedReadProgress)
        ctx.percent = qBound(0, ReadingView_getCalculatedReadProgress(rv), 100);

    if (selection && WebkitView_selectedText) {
        if (QWidget *wv = find_webkit_view(rv)) {
            QString text;
            WebkitView_selectedText(&text, wv);
            ctx.selection = text.simplified();
        } else {
            ngpt_log("context: no WebkitView for the selection");
        }
    }

    ngpt_log("context: id=%s series=\"%s\" #%s title=\"%s\" chapter=\"%s\" pct=%d sel=%d chars",
           qPrintable(ctx.contentId.left(40)), qPrintable(ctx.series), qPrintable(ctx.seriesNumber),
           qPrintable(ctx.title.left(40)), qPrintable(ctx.chapter.left(40)), ctx.percent, ctx.selection.size());
    return ctx;
}
