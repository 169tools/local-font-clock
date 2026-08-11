/*
Oshi-Font Clock
Copyright (C) 2026 mizznoff <mizznoff@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "font-dialog.hpp"

#include "layout.hpp"
#include "text-renderer.hpp"

#include <obs-frontend-api.h>

#include <QDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QVBoxLayout>

#include <memory>

namespace {

/* Bold reads better at a glance than regular, which is what a clock is for, so
 * it is what a family defaults to when it has one. */
const char *preferred_style = "Bold";

/* Small enough to sit beside the style list, large enough to judge a typeface
 * by. The date row follows the same proportion the clock itself uses. */
constexpr double preview_ink_height = 30.0;

/// Draws the actual clock, not a specimen string. Choosing a font for this
/// plugin is choosing how the clock will look, so showing anything else asks
/// the user to imagine the part that matters.
QPixmap render_preview(const QString &family, const QString &style)
{
	clock_style spec;
	spec.face = family.toStdString();
	spec.style = style.toStdString();
	spec.time_ink_height = preview_ink_height;
	spec.date_ink_height = preview_ink_height * 18.0 / 44.0;
	spec.color = 0xffffffff;

	/* Prepared and discarded per thumbnail: the point of the preview is that the
	 * face keeps changing, so there is nothing to hold on to between them. */
	const std::unique_ptr<prepared_clock> clock = prepare_clock(spec);
	if (!clock)
		return {};

	const rendered_text bitmap = clock->render({"12:34", "5/6 WED"});
	if (!bitmap.valid())
		return {};

	const QImage image(bitmap.pixels.data(), static_cast<int>(bitmap.width), static_cast<int>(bitmap.height),
			   static_cast<int>(bitmap.width) * 4, QImage::Format_RGBA8888_Premultiplied);

	/* The clock carries its own outer margin, which is right on screen and just
	 * wasted space in a thumbnail -- the frame around the label already reads as
	 * padding. Trimmed back to a couple of pixels, which the date's descenders
	 * still need since they reach past the digits. */
	constexpr int keep = 2;
	const int trim_x = static_cast<int>(margin_horizontal_ratio * preview_ink_height) - keep;
	const int trim_y = static_cast<int>(margin_vertical_ratio * preview_ink_height) - keep;

	const QRect content = image.rect().adjusted(qMax(trim_x, 0), qMax(trim_y, 0), -qMax(trim_x, 0),
						    -qMax(trim_y, 0));

	return QPixmap::fromImage(image.copy(content));
}

QString pick_default_style(const QStringList &styles, const QString &wanted)
{
	if (styles.contains(wanted))
		return wanted;
	if (styles.contains(preferred_style))
		return preferred_style;
	return styles.isEmpty() ? QString() : styles.first();
}

} // namespace

bool choose_font(std::string &face, std::string &style)
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog dialog(parent);
	dialog.setWindowTitle(QStringLiteral("Font"));
	dialog.resize(480, 560);

	/* Stacked rather than side by side: the family list is the one that needs
	 * the room, and styles are a short list under whatever is selected. */
	auto *layout = new QVBoxLayout(&dialog);

	layout->addWidget(new QLabel(QStringLiteral("Font"), &dialog));

	auto *families = new QListWidget(&dialog);
	families->addItems(QFontDatabase::families());
	layout->addWidget(families, 1);

	layout->addWidget(new QLabel(QStringLiteral("Font style"), &dialog));

	/* Styles take the left half; the right is given over to the preview, which
	 * is the thing actually being judged. */
	auto *lower = new QHBoxLayout;
	layout->addLayout(lower);

	auto *styles = new QListWidget(&dialog);
	lower->addWidget(styles, 1);

	auto *preview = new QLabel(&dialog);
	preview->setAlignment(Qt::AlignCenter);
	preview->setFrameStyle(QFrame::Sunken | QFrame::Panel);
	lower->addWidget(preview, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addWidget(buttons);

	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	const QString wanted_style = QString::fromStdString(style);

	/* Four rows covers the usual Regular / Bold / Italic / Bold Italic spread.
	 * Holding the styles to a fixed height leaves the rest of the dialog to the
	 * family list, which is the one that needs room to scroll. The row height
	 * has to be measured rather than derived from the font: list items carry
	 * padding of their own, and OBS restyles them. */
	const int visible_style_rows = 4;

	QObject::connect(families, &QListWidget::currentTextChanged, styles,
			 [styles, wanted_style, visible_style_rows](const QString &family) {
				 /* Carry the weight across families that have it, so browsing
				  * compares like with like. Read from the live selection rather
				  * than from what the dialog opened with, or a weight the user
				  * picked by hand would be thrown away on the next family. */
				 const QString previous = styles->currentItem() ? styles->currentItem()->text()
									       : wanted_style;

				 const QStringList available = QFontDatabase::styles(family);
				 styles->clear();
				 styles->addItems(available);

				 if (styles->count() > 0)
					 styles->setFixedHeight(styles->sizeHintForRow(0) * visible_style_rows +
								styles->frameWidth() * 2);

				 const QString chosen = pick_default_style(available, previous);
				 const auto matches = styles->findItems(chosen, Qt::MatchExactly);
				 if (!matches.isEmpty())
					 styles->setCurrentItem(matches.first());
			 });

	const auto refresh_preview = [families, styles, preview]() {
		if (!families->currentItem())
			return;

		const QString style_name = styles->currentItem() ? styles->currentItem()->text() : QString();
		const QPixmap pixmap = render_preview(families->currentItem()->text(), style_name);

		if (pixmap.isNull())
			preview->setText(QStringLiteral("0123456789:/"));
		else
			preview->setPixmap(pixmap);
	};

	QObject::connect(families, &QListWidget::currentTextChanged, preview, refresh_preview);
	QObject::connect(styles, &QListWidget::currentTextChanged, preview, refresh_preview);

	const auto current = families->findItems(QString::fromStdString(face), Qt::MatchExactly);
	if (!current.isEmpty())
		families->setCurrentItem(current.first());
	else if (families->count() > 0)
		families->setCurrentRow(0);

	if (dialog.exec() != QDialog::Accepted)
		return false;

	if (!families->currentItem())
		return false;

	face = families->currentItem()->text().toStdString();
	style = styles->currentItem() ? styles->currentItem()->text().toStdString() : std::string();
	return true;
}
