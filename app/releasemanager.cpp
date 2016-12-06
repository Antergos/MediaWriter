/*
 * Fedora Media Writer
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "releasemanager.h"

#include "isomd5/libcheckisomd5.h"

#include <QtQml>
#include <QApplication>
#include <QAbstractEventDispatcher>

#include <QJsonDocument>

ReleaseManager::ReleaseManager(QObject *parent)
    : QSortFilterProxyModel(parent), m_sourceModel(new ReleaseListModel(this))
{
    setSourceModel(m_sourceModel);

    qmlRegisterUncreatableType<Release>("MediaWriter", 1, 0, "Release", "");
    qmlRegisterUncreatableType<ReleaseVersion>("MediaWriter", 1, 0, "Version", "");
    qmlRegisterUncreatableType<ReleaseVariant>("MediaWriter", 1, 0, "Variant", "");
    qmlRegisterUncreatableType<ReleaseArchitecture>("MediaWriter", 1, 0, "Architecture", "");
    qmlRegisterUncreatableType<Progress>("MediaWriter", 1, 0, "Progress", "");

    QFile releases(":/releases.json");
    releases.open(QIODevice::ReadOnly);
    onStringDownloaded(releases.readAll());
    releases.close();

    QTimer::singleShot(0, this, &ReleaseManager::fetchReleases);
}

bool ReleaseManager::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const {
    Q_UNUSED(source_parent)
    if (m_frontPage)
        if (source_row < 3)
            return true;
        else
            return false;
    else {
        auto r = get(source_row);
        bool containsArch = false;
        for (auto version : r->versionList()) {
            for (auto variant : version->variantList()) {
                if (variant->arch()->index() == m_filterArchitecture) {
                    containsArch = true;
                    break;
                }
            }
            if (containsArch)
                break;
        }
        return r->isLocal() || (containsArch && (r->name().contains(m_filterText, Qt::CaseInsensitive) || r->summary().contains(m_filterText, Qt::CaseInsensitive)));
    }
}

Release *ReleaseManager::get(int index) const {
    return m_sourceModel->get(index);
}

void ReleaseManager::fetchReleases() {
    m_beingUpdated = true;
    emit beingUpdatedChanged();

    DownloadManager::instance()->fetchPageAsync(this, "https://build.antergos.com/api/releases.json");
}

bool ReleaseManager::beingUpdated() const {
    return m_beingUpdated;
}

bool ReleaseManager::frontPage() const {
    return m_frontPage;
}

void ReleaseManager::setFrontPage(bool o) {
    if (m_frontPage != o) {
        m_frontPage = o;
        emit frontPageChanged();
        invalidateFilter();
    }
}

QString ReleaseManager::filterText() const {
    return m_filterText;
}

void ReleaseManager::setFilterText(const QString &o) {
    if (m_filterText != o) {
        m_filterText = o;
        emit filterTextChanged();
        invalidateFilter();
    }
}

void ReleaseManager::setLocalFile(const QString &path) {
    for (int i = 0; i < m_sourceModel->rowCount(); i++) {
        Release *r = m_sourceModel->get(i);
        if (r->source() == Release::LOCAL) {
            r->setLocalFile(path);
        }
    }
}

bool ReleaseManager::updateUrl(const QString &release, int version, const QString &status, const QDateTime &releaseDate, const QString &architecture, const QString &url, const QString &sha256, int64_t size) {
    for (int i = 0; i < m_sourceModel->rowCount(); i++) {
        Release *r = get(i);
        if (r->name().toLower().contains(release))
            return r->updateUrl(version, status, releaseDate, architecture, url, sha256, size);
    }
    return false;
}

int ReleaseManager::filterArchitecture() const {
    return m_filterArchitecture;
}

void ReleaseManager::setFilterArchitecture(int o) {
    if (m_filterArchitecture != o && m_filterArchitecture >= 0 && m_filterArchitecture < ReleaseArchitecture::_ARCHCOUNT) {
        m_filterArchitecture = o;
        emit filterArchitectureChanged();
        invalidateFilter();
    }
}

Release *ReleaseManager::selected() const {
    if (m_selectedIndex >= 0 && m_selectedIndex < m_sourceModel->rowCount())
        return m_sourceModel->get(m_selectedIndex);
    return nullptr;
}

int ReleaseManager::selectedIndex() const {
    return m_selectedIndex;
}

void ReleaseManager::setSelectedIndex(int o) {
    if (m_selectedIndex != o) {
        m_selectedIndex = o;
        emit selectedChanged();
    }
}

void ReleaseManager::onStringDownloaded(const QString &text) {
    QRegExp re("(\\d+)\\s?(\\S+)?");
    auto doc = QJsonDocument::fromJson(text.toUtf8());

    for (auto i : doc.array()) {
        QJsonObject obj = i.toObject();
        QString arch = obj["arch"].toString().toLower();
        QString url = obj["link"].toString();
        QString release = obj["subvariant"].toString().toLower();
        QString versionWithStatus = obj["version"].toString().toLower();
        QString sha256 = obj["sha256"].toString();
        QDateTime releaseDate = QDateTime::fromString((obj["releaseDate"].toString()), "yyyy-MM-dd");
        int64_t size = obj["size"].toString().toLongLong();
        int version;
        QString status;

        if (QStringList{"cloud", "cloud_base", "atomic", "everything", "minimal", "docker", "docker_base"}.contains(release))
            continue;

        release.replace(QRegExp("_kde$"), "");
        release.replace("_", " ");

        if (arch == "armhfp")
            continue;

        if (re.indexIn(versionWithStatus) < 0)
            continue;

        if (release.contains("workstation") && !url.contains("Live"))
            continue;

        if (release.contains("server") && !url.contains("dvd"))
            continue;

        version = re.capturedTexts()[1].toInt();
        status = re.capturedTexts()[2];

        if (!release.isEmpty() && !url.isEmpty() && !arch.isEmpty())
            updateUrl(release, version, status, releaseDate, arch, url, sha256, size);
    }

    m_beingUpdated = false;
    emit beingUpdatedChanged();
}

void ReleaseManager::onDownloadError(const QString &message) {
    qWarning() << "Was not able to fetch new releases:" << message << "Retrying in 10 seconds.";

    QTimer::singleShot(10000, this, &ReleaseManager::fetchReleases);
}

QStringList ReleaseManager::architectures() const {
    return ReleaseArchitecture::listAllDescriptions();
}


QVariant ReleaseListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    Q_UNUSED(section); Q_UNUSED(orientation);

    if (role == Qt::UserRole + 1)
        return "release";

    return QVariant();
}

QHash<int, QByteArray> ReleaseListModel::roleNames() const {
    QHash<int, QByteArray> ret;
    ret.insert(Qt::UserRole + 1, "release");
    return ret;
}

int ReleaseListModel::rowCount(const QModelIndex &parent) const {
    Q_UNUSED(parent)
    return m_releases.count();
}

QVariant ReleaseListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return QVariant();

    if (role == Qt::UserRole + 1)
        return QVariant::fromValue(m_releases[index.row()]);

    return QVariant();
}


ReleaseListModel::ReleaseListModel(ReleaseManager *parent)
    : QAbstractListModel(parent) {
    QFile metadata(":/metadata.json");
    metadata.open(QIODevice::ReadOnly);

    Release *custom = nullptr;

    auto doc = QJsonDocument::fromJson(metadata.readAll());
    for (auto i : doc.array()) {
        QJsonObject obj = i.toObject();
        QString subvariant = obj["subvariant"].toString();
        QString sourceString = obj["category"].toString();
        Release::Source source = sourceString == "product" ? Release::PRODUCT :
                                 sourceString == "spins"   ? Release::SPINS :
                                 sourceString == "labs"    ? Release::LABS :
                                                             Release::OTHER;
        QString name = obj["name"].toString();
        QString summary = obj["summary"].toString();
        QStringList description;
        for (auto j : obj["description"].toArray()) {
            description.append(j.toString());
        }
        QStringList screenshots;
        for (auto j : obj["screenshots"].toArray()) {
            screenshots.append(j.toString());
        }
        QString icon = "qrc:/logos/placeholder";
        if (obj.contains("icon"))
            icon = obj["icon"].toString();

        m_releases.append(new Release(manager(), m_releases.count(), name, summary, description, source, icon, screenshots));
        if (m_releases.count() == 2) {
            custom = new Release (manager(), 2, tr("Custom image"), QT_TRANSLATE_NOOP("Release", "Pick a file from your drive(s)"), { QT_TRANSLATE_NOOP("Release", "<p>Here you can choose a OS image from your hard drive to be written to your flash disk</p><p>Currently it is only supported to write raw disk images (.iso or .bin)</p>") }, Release::LOCAL, "qrc:/logos/folder", {});
            m_releases.append(custom);
        }
    }

    if (m_releases.count() < 2) {
        custom = new Release (manager(), m_releases.count(), tr("Custom image"), QT_TRANSLATE_NOOP("Release", "Pick a file from your drive(s)"), { QT_TRANSLATE_NOOP("Release", "<p>Here you can choose a OS image from your hard drive to be written to your flash disk</p><p>Currently it is only supported to write raw disk images (.iso or .bin)</p>") }, Release::LOCAL, "qrc:/logos/folder", {});
        m_releases.append(custom);
    }

    ReleaseVersion *customVersion = new ReleaseVersion(custom, 0);
    custom->addVersion(customVersion);
    customVersion->addVariant(new ReleaseVariant(customVersion, QString(), QString(), 0, ReleaseArchitecture::fromId(ReleaseArchitecture::X86_64)));
}

ReleaseManager *ReleaseListModel::manager() {
    return qobject_cast<ReleaseManager*>(parent());
}

Release *ReleaseListModel::get(int index) {
    if (index >= 0 && index < m_releases.count())
        return m_releases[index];
    return nullptr;
}


QString Release::sourceString() {
    switch (m_source) {
    case LOCAL:
    case PRODUCT:
        return QString();
    case SPINS:
        return tr("Fedora Spins");
    case LABS:
        return tr("Fedora Labs");
    default:
        return tr("Other");
    }
}

int Release::index() const {
    return m_index;
}

Release::Release(ReleaseManager *parent, int index, const QString &name, const QString &summary, const QStringList &description, Release::Source source, const QString &icon, const QStringList &screenshots)
    : QObject(parent), m_index(index), m_name(name), m_summary(summary), m_description(description), m_source(source), m_icon(icon), m_screenshots(screenshots)
{

}

void Release::setLocalFile(const QString &path) {
    if (m_source != LOCAL)
        return;

    QFileInfo info(QUrl(path).toLocalFile());

    if (!info.exists()) {
        qCritical() << path << "doesn't exist";
        return;
    }

    if (m_versions.count() == 1) {
        m_versions.first()->deleteLater();
        m_versions.removeFirst();
    }

    m_versions.append(new ReleaseVersion(this, QUrl(path).toLocalFile(), info.size()));
    emit versionsChanged();
    emit selectedVersionChanged();
}

bool Release::updateUrl(int version, const QString &status, const QDateTime &releaseDate, const QString &architecture, const QString &url, const QString &sha256, int64_t size) {
    for (auto i : m_versions) {
        if (i->number() == version)
            return i->updateUrl(status, releaseDate, architecture, url, sha256, size);
    }
    ReleaseVersion::Status s = status == "alpha" ? ReleaseVersion::ALPHA : status == "beta" ? ReleaseVersion::BETA : ReleaseVersion::FINAL;
    auto ver = new ReleaseVersion(this, version, s, releaseDate);
    auto variant = new ReleaseVariant(ver, url, sha256, size, ReleaseArchitecture::fromAbbreviation(architecture));
    ver->addVariant(variant);
    addVersion(ver);
    return true;
}

QString Release::name() const {
    return m_name;
}

QString Release::summary() const {
    return tr(m_summary.toUtf8());
}

QString Release::description() const {
    QString result;
    for (auto i : m_description) {
        // there is a %(rel)s formatting string in the translation texts, get rid of that
        // get rid of in-translation break tags too
        result.append(tr(i.toUtf8()).replace("\%(rel)s ", "").replace("<br />", ""));
    }
    return result;
}

Release::Source Release::source() const {
    return m_source;
}

bool Release::isLocal() const {
    return m_source == Release::LOCAL;
}

QString Release::icon() const {
    return m_icon;
}

QStringList Release::screenshots() const {
    return m_screenshots;
}

QString Release::prerelease() const {
    if (m_versions.empty() || m_versions.first()->status() == ReleaseVersion::FINAL)
        return "";
    return m_versions.first()->name();
}

QQmlListProperty<ReleaseVersion> Release::versions() {
    return QQmlListProperty<ReleaseVersion>(this, m_versions);
}

QList<ReleaseVersion *> Release::versionList() const {
    return m_versions;
}

QStringList Release::versionNames() const {
    QStringList ret;
    for (auto i : m_versions) {
        ret.append(i->name());
    }
    return ret;
}

void Release::addVersion(ReleaseVersion *version) {
    for (int i = 0; i < m_versions.count(); i++) {
        if (m_versions[i]->number() < version->number()) {
            m_versions.insert(i, version);
            emit versionsChanged();
            if (version->status() != ReleaseVersion::FINAL && m_selectedVersion >= i) {
                m_selectedVersion++;
            }
            emit selectedVersionChanged();
            return;
        }
    }
    m_versions.append(version);
    emit versionsChanged();
    emit selectedVersionChanged();
}

ReleaseVersion *Release::selectedVersion() const {
    if (m_selectedVersion >= 0 && m_selectedVersion < m_versions.count())
        return m_versions[m_selectedVersion];
    return nullptr;
}

int Release::selectedVersionIndex() const {
    return m_selectedVersion;
}

void Release::setSelectedVersionIndex(int o) {
    if (m_selectedVersion != o && m_selectedVersion >= 0 && m_selectedVersion < m_versions.count()) {
        m_selectedVersion = o;
        emit selectedVersionChanged();
    }
}


ReleaseVersion::ReleaseVersion(Release *parent, int number, ReleaseVersion::Status status, QDateTime releaseDate)
    : QObject(parent), m_number(number), m_status(status), m_releaseDate(releaseDate)
{
    if (status != FINAL)
        emit parent->prereleaseChanged();
}

ReleaseVersion::ReleaseVersion(Release *parent, const QString &file, int64_t size)
    : QObject(parent), m_variants({ new ReleaseVariant(this, file, size) })
{

}

Release *ReleaseVersion::release() {
    return qobject_cast<Release*>(parent());
}

bool ReleaseVersion::updateUrl(const QString &status, const QDateTime &releaseDate, const QString &architecture, const QString &url, const QString &sha256, int64_t size) {
    Status s = status == "alpha" ? ALPHA : status == "beta" ? BETA : FINAL;
    if (s <= m_status) {
        m_status = s;
        emit statusChanged();
        if (s == FINAL)
            emit release()->prereleaseChanged();
    }
    else {
        return false;
    }
    if (m_releaseDate != releaseDate && releaseDate.isValid()) {
        m_releaseDate = releaseDate;
        emit releaseDateChanged();
    }
    for (auto i : m_variants) {
        if (i->arch() == ReleaseArchitecture::fromAbbreviation(architecture))
            return i->updateUrl(url, sha256, size);
    }
    m_variants.append(new ReleaseVariant(this, url, sha256, size, ReleaseArchitecture::fromAbbreviation(architecture)));
    return true;
}

int ReleaseVersion::number() const {
    return m_number;
}

QString ReleaseVersion::name() const {
    switch (m_status) {
    case ALPHA:
        return tr("%1 Alpha").arg(m_number);
    case BETA:
        return tr("%1 Beta").arg(m_number);
    case RELEASE_CANDIDATE:
        return tr("%1 Release Candidate").arg(m_number);
    default:
        return QString("%1").arg(m_number);
    }
}

ReleaseVariant *ReleaseVersion::selectedVariant() const {
    if (m_selectedVariant >= 0 && m_selectedVariant < m_variants.count())
        return m_variants[m_selectedVariant];
    return nullptr;
}

int ReleaseVersion::selectedVariantIndex() const {
    return m_selectedVariant;
}

void ReleaseVersion::setSelectedVariantIndex(int o) {
    if (m_selectedVariant != o && m_selectedVariant >= 0 && m_selectedVariant < m_variants.count()) {
        m_selectedVariant = o;
        emit selectedVariantChanged();
    }
}

ReleaseVersion::Status ReleaseVersion::status() const {
    return m_status;
}

QDateTime ReleaseVersion::releaseDate() const {
    return m_releaseDate;
}

void ReleaseVersion::addVariant(ReleaseVariant *v) {
    m_variants.append(v);
    emit variantsChanged();
    if (m_variants.count() == 1)
        emit selectedVariantChanged();
}

QQmlListProperty<ReleaseVariant> ReleaseVersion::variants() {
    return QQmlListProperty<ReleaseVariant>(this, m_variants);
}

QList<ReleaseVariant *> ReleaseVersion::variantList() const {
    return m_variants;
}


ReleaseVariant::ReleaseVariant(ReleaseVersion *parent, QString url, QString shaHash, int64_t size, ReleaseArchitecture *arch, ReleaseVariant::Type type)
    : QObject(parent), m_arch(arch), m_type(type), m_url(url), m_shaHash(shaHash), m_size(size)
{

}

ReleaseVariant::ReleaseVariant(ReleaseVersion *parent, const QString &file, int64_t size)
    : QObject(parent), m_iso(file), m_arch(ReleaseArchitecture::fromId(ReleaseArchitecture::X86_64)), m_size(size)
{
    m_status = READY;
}

bool ReleaseVariant::updateUrl(const QString &url, const QString &sha256, int64_t size) {
    bool changed = false;
    if (!url.isEmpty() && m_url.toUtf8().trimmed() != url.toUtf8().trimmed()) {
        qWarning() << "Url" << m_url << "changed to" << url;
        m_url = url;
        emit urlChanged();
        changed = true;
    }
    if (!sha256.isEmpty() && m_shaHash.trimmed() != sha256.trimmed()) {
        qWarning() << "SHA256 hash of" << url << "changed from" << m_shaHash << "to" << sha256;
        m_shaHash = sha256;
        emit shaHashChanged();
        changed = true;
    }
    if (size != 0 && m_size != size) {
        m_size = size;
        emit sizeChanged();
        changed = true;
    }
    return changed;
}

ReleaseVersion *ReleaseVariant::releaseVersion() {
    return qobject_cast<ReleaseVersion*>(parent());
}

Release *ReleaseVariant::release() {
    return releaseVersion()->release();
}

ReleaseArchitecture *ReleaseVariant::arch() const {
    return m_arch;
}

ReleaseVariant::Type ReleaseVariant::type() const {
    return m_type;
}

QString ReleaseVariant::name() const {
    return m_arch->description();
}

QString ReleaseVariant::url() const {
    return m_url;
}

QString ReleaseVariant::shaHash() const {
    return m_shaHash;
}

QString ReleaseVariant::iso() const {
    return m_iso;
}

qreal ReleaseVariant::size() const {
    return m_size;
}

Progress *ReleaseVariant::progress() {
    if (!m_progress)
        m_progress = new Progress(this, 0.0, size());

    return m_progress;
}

ReleaseVariant::Status ReleaseVariant::status() const {
    return m_status;
}

QString ReleaseVariant::statusString() const {
    return m_statusStrings[m_status];
}

void ReleaseVariant::onFileDownloaded(const QString &path, const QString &hash) {
    m_iso = path;
    emit isoChanged();

    if (m_progress)
        m_progress->setValue(size());
    setStatus(DOWNLOAD_VERIFYING);
    m_progress->setValue(0.0/0.0, 1.0);

    if (!shaHash().isEmpty() && shaHash() != hash) {
        qWarning() << "Computed SHA256 hash of" << path << " - " << hash << "does not match expected" << shaHash();
        setErrorString(tr("The downloaded image is corrupted"));
        setStatus(FAILED_DOWNLOAD);
    }

    qApp->eventDispatcher()->processEvents(QEventLoop::AllEvents);

    int checkResult = mediaCheckFile(QDir::toNativeSeparators(path).toLocal8Bit(), &ReleaseVariant::staticOnMediaCheckAdvanced, this);
    if (checkResult == ISOMD5SUM_CHECK_FAILED) {
        qWarning() << "Internal MD5 media check of" << path << "failed with status" << checkResult;
        QFile::remove(path);
        setErrorString(tr("The downloaded image is corrupted"));
        setStatus(FAILED_DOWNLOAD);
    }
    else if (checkResult == ISOMD5SUM_FILE_NOT_FOUND) {
        setErrorString(tr("The downloaded file is not readable."));
        setStatus(FAILED_DOWNLOAD);
    }
    else {
        setStatus(READY);

        if (QFile(m_iso).size() != m_size) {
            m_size = QFile(m_iso).size();
            emit sizeChanged();
        }
    }
}

void ReleaseVariant::onDownloadError(const QString &message) {
    setErrorString(message);
    setStatus(FAILED_DOWNLOAD);
}

int ReleaseVariant::staticOnMediaCheckAdvanced(void *data, long long offset, long long total) {
    ReleaseVariant *v = static_cast<ReleaseVariant*>(data);
    return v->onMediaCheckAdvanced(offset, total);
}

int ReleaseVariant::onMediaCheckAdvanced(long long offset, long long total) {
    qApp->eventDispatcher()->processEvents(QEventLoop::AllEvents);
    m_progress->setValue(offset, total);
    return 0;
}

void ReleaseVariant::download() {
    if (url().isEmpty() && !iso().isEmpty()) {
        setStatus(READY);
    }
    else {
        resetStatus();
        setStatus(DOWNLOADING);
        if (m_size)
            m_progress->setTo(m_size);
        QString ret = DownloadManager::instance()->downloadFile(this, url(), DownloadManager::dir(), progress());
        if (!ret.isEmpty()) {
            m_iso = ret;
            emit isoChanged();

            setStatus(READY);

            if (QFile(m_iso).size() != m_size) {
                m_size = QFile(m_iso).size();
                emit sizeChanged();
            }
        }
    }
}

void ReleaseVariant::resetStatus() {
    if (!m_iso.isEmpty()) {
        setStatus(READY);
    }
    else {
        setStatus(PREPARING);
        if (m_progress)
            m_progress->setValue(0.0);
    }
    setErrorString(QString());
    emit statusChanged();
}

void ReleaseVariant::setStatus(Status s) {
    if (m_status != s) {
        m_status = s;
        emit statusChanged();
    }
}

QString ReleaseVariant::errorString() const {
    return m_error;
}

void ReleaseVariant::setErrorString(const QString &o) {
    if (m_error != o) {
        m_error = o;
        emit errorStringChanged();
    }
}


ReleaseArchitecture ReleaseArchitecture::m_all[] = {
    {{"x86_64"}, QT_TR_NOOP("Intel 64bit"), QT_TR_NOOP("ISO format image for Intel, AMD and other compatible PCs (64-bit)")},
    {{"x86", "i386", "i686"}, QT_TR_NOOP("Intel 32bit"), QT_TR_NOOP("ISO format image for Intel, AMD and other compatible PCs (32-bit)")},
    {{"armv7hl", "armhfp"}, QT_TR_NOOP("ARM v7"), QT_TR_NOOP("LZMA-compressed raw image for ARM v7-A machines like the Raspberry Pi 2 and 3")},
};

ReleaseArchitecture::ReleaseArchitecture(const QStringList &abbreviation, const char *description, const char *details)
    : m_abbreviation(abbreviation), m_description(description), m_details(details)
{

}

ReleaseArchitecture *ReleaseArchitecture::fromId(ReleaseArchitecture::Id id) {
    if (id >= 0 && id < _ARCHCOUNT)
        return &m_all[id];
    return nullptr;
}

ReleaseArchitecture *ReleaseArchitecture::fromAbbreviation(const QString &abbr) {
    for (int i = 0; i < _ARCHCOUNT; i++) {
        if (m_all[i].abbreviation().contains(abbr, Qt::CaseInsensitive))
            return &m_all[i];
    }
    return nullptr;
}

QList<ReleaseArchitecture *> ReleaseArchitecture::listAll() {
    QList<ReleaseArchitecture *> ret;
    for (int i = 0; i < _ARCHCOUNT; i++) {
        ret.append(&m_all[i]);
    }
    return ret;
}

QStringList ReleaseArchitecture::listAllDescriptions() {
    QStringList ret;
    for (int i = 0; i < _ARCHCOUNT; i++) {
        ret.append(m_all[i].description());
    }
    return ret;
}

QStringList ReleaseArchitecture::abbreviation() const {
    return m_abbreviation;
}

QString ReleaseArchitecture::description() const {
    return tr(m_description);
}

QString ReleaseArchitecture::details() const {
    return tr(m_details);
}

int ReleaseArchitecture::index() const {
    return this - m_all;
}
