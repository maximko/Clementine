/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "spotifyblobdownloader.h"
#include "spotifyservice.h"
#include "core/logging.h"
#include "core/network.h"
#include "core/utilities.h"

#include <QDir>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QtCrypto>

const char* SpotifyBlobDownloader::kSignatureSuffix = ".sha1";


SpotifyBlobDownloader::SpotifyBlobDownloader(
      const QString& version, const QString& path, QObject* parent)
  : QObject(parent),
    version_(version),
    path_(path),
    network_(new NetworkAccessManager(this)),
    progress_(new QProgressDialog(tr("Downloading Spotify plugin"), tr("Cancel"), 0, 0))
{
  progress_->setWindowTitle(QCoreApplication::applicationName());
  connect(progress_, SIGNAL(canceled()), SLOT(Cancel()));
}

SpotifyBlobDownloader::~SpotifyBlobDownloader() {
  qDeleteAll(replies_);
  replies_.clear();

  delete progress_;
}

bool SpotifyBlobDownloader::Prompt() {
  QMessageBox::StandardButton ret = QMessageBox::question(NULL,
      tr("Spotify plugin not installed"),
      tr("An additional plugin is required to use Spotify in Clementine.  Would you like to download and install it now?"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
  return ret == QMessageBox::Yes;
}

void SpotifyBlobDownloader::Start() {
  qDeleteAll(replies_);
  replies_.clear();

  const QStringList filenames = QStringList()
      << "blob"
      << "blob" + QString(kSignatureSuffix)
      << "libspotify.so.8";

  foreach (const QString& filename, filenames) {
    const QUrl url(SpotifyService::kBlobDownloadUrl + version_ + "/" + filename);
    qLog(Info) << "Downloading" << url;

    QNetworkReply* reply = network_->get(QNetworkRequest(url));
    connect(reply, SIGNAL(finished()), SLOT(ReplyFinished()));
    connect(reply, SIGNAL(downloadProgress(qint64,qint64)), SLOT(ReplyProgress()));

    replies_ << reply;
  }

  progress_->show();
}

void SpotifyBlobDownloader::ReplyFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (reply->error() != QNetworkReply::NoError) {
    // Handle network errors
    ShowError(reply->errorString());
    return;
  }

  // Is everything finished?
  foreach (QNetworkReply* reply, replies_) {
    if (!reply->isFinished()) {
      return;
    }
  }

  // Let's verify signatures in a temporary directory first, before we write
  // anything into its final position.
  QString temp_directory = Utilities::MakeTempDir();
  QStringList signatures;

  foreach (QNetworkReply* reply, replies_) {
    const QString filename = reply->url().path().section('/', -1, -1);
    const QString path = temp_directory + "/" + filename;

    qLog(Info) << "Saving file" << path;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
      ShowError("Failed to open file for writing: " + path);
      return;
    }

    if (path.endsWith(kSignatureSuffix)) {
      signatures << path;
    }

    file.setPermissions(QFile::Permissions(0x7755));
    file.write(reply->readAll());
  }

  // Load the public key
  QCA::ConvertResult conversion_result;
  QCA::PublicKey key = QCA::PublicKey::fromPEMFile(":/clementine-spotify-public.pem",
                                                   &conversion_result);
  if (QCA::ConvertGood != conversion_result) {
    ShowError("Failed to load Spotify public key");
    return;
  }

  // Verify signatures
  foreach (const QString& signature_filename, signatures) {
    QString filename = signature_filename;
    filename.remove(kSignatureSuffix);

    qLog(Debug) << "Verifying" << filename << "against" << signature_filename;

    QFile actual_file(filename);
    if (!actual_file.open(QIODevice::ReadOnly))
      return;

    QFile signature_file(signature_filename);
    if (!signature_file.open(QIODevice::ReadOnly))
      return;

    if (!key.verifyMessage(actual_file.readAll(), signature_file.readAll(),
                           QCA::EMSA3_SHA1)) {
      ShowError("Invalid signature: " + filename);
      return;
    }

    qLog(Debug) << "Verification OK";
  }

  // Make the destination directory and write the files into it
  QDir().mkpath(path_);

  foreach (QNetworkReply* reply, replies_) {
    const QString filename = reply->url().path().section('/', -1, -1);
    const QString source_path = temp_directory + "/" + filename;
    const QString dest_path = path_ + "/" + filename;

    if (filename.endsWith(kSignatureSuffix))
      continue;

    qLog(Info) << "Moving" << source_path << "to" << dest_path;

    if (!QFile::rename(source_path, dest_path)) {
      ShowError("Writing file failed: " + dest_path);
      return;
    }
  }

  // Remove the temporary directory
  Utilities::RemoveRecursive(temp_directory);

  EmitFinished();
}

void SpotifyBlobDownloader::ReplyProgress() {
  int progress = 0;
  int total = 0;

  foreach (QNetworkReply* reply, replies_) {
    progress += reply->bytesAvailable();
    total += reply->rawHeader("Content-Length").toInt();
  }

  progress_->setMaximum(total);
  progress_->setValue(progress);
}

void SpotifyBlobDownloader::Cancel() {
  deleteLater();
}

void SpotifyBlobDownloader::ShowError(const QString& message) {
  // Stop any remaining replies before showing the dialog so they don't
  // carry on in the background
  foreach (QNetworkReply* reply, replies_) {
    disconnect(reply, 0, this, 0);
    reply->abort();
  }

  qLog(Warning) << message;
  QMessageBox::warning(NULL, tr("Error downloading Spotify plugin"), message,
                       QMessageBox::Close);
  deleteLater();
}

void SpotifyBlobDownloader::EmitFinished() {
  emit Finished();
  deleteLater();
}
