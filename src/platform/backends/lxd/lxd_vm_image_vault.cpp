/*
 * Copyright (C) 2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "lxd_vm_image_vault.h"
#include "lxd_request.h"

#include <multipass/exceptions/aborted_download_exception.h>
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/network_access_manager.h>
#include <multipass/platform.h>
#include <multipass/rpc/multipass.grpc.pb.h>
#include <multipass/url_downloader.h>
#include <multipass/utils.h>
#include <multipass/vm_image.h>
#include <multipass/vm_image_host.h>

#include <shared/linux/backend_utils.h>
#include <shared/linux/process_factory.h>

#include <yaml-cpp/yaml.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTemporaryDir>

#include <chrono>
#include <thread>

namespace mp = multipass;
namespace mpl = multipass::logging;

using namespace std::literals::chrono_literals;

namespace
{
constexpr auto category = "lxd image vault";

const QHash<QString, QString> host_to_lxd_arch{{"x86_64", "x86_64"}, {"arm", "armv7l"}, {"arm64", "aarch64"},
                                               {"i386", "i686"},     {"power", "ppc"},  {"power64", "ppc64"},
                                               {"s390x", "s390x"}};

auto parse_percent_as_int(const QString& progress_string)
{
    QRegularExpression re{"rootfs:\\s(\\d{1,3})%"};

    QRegularExpressionMatch match = re.match(progress_string);

    if (match.hasMatch())
    {
        return match.captured(1).toInt();
    }

    return -1;
}

QString post_process_downloaded_image(const QString& image_path, const mp::ProgressMonitor& monitor)
{
    QString new_image_path{image_path};

    if (image_path.endsWith(".xz"))
    {
        new_image_path = mp::vault::extract_image(image_path, monitor, true);
    }

    QString original_image_path{new_image_path};
    new_image_path = mp::backend::convert_to_qcow_if_necessary(new_image_path);

    if (original_image_path != new_image_path)
    {
        mp::vault::delete_file(original_image_path);
    }

    return new_image_path;
}

QString create_metadata_tarball(const mp::VMImageInfo& info, const QTemporaryDir& lxd_import_dir)
{
    QFile metadata_yaml_file{lxd_import_dir.filePath("metadata.yaml")};
    YAML::Node metadata_node;

    metadata_node["architecture"] = host_to_lxd_arch.value(QSysInfo::currentCpuArchitecture()).toStdString();
    metadata_node["creation_date"] = QDateTime::currentSecsSinceEpoch();
    metadata_node["properties"]["description"] = info.release_title.toStdString();
    metadata_node["properties"]["os"] = info.os.toStdString();
    metadata_node["properties"]["release"] = info.release.toStdString();
    metadata_node["properties"]["version"] = info.version.toStdString();
    metadata_node["properties"]["original_hash"] = info.id.toStdString();

    YAML::Emitter emitter;
    emitter << metadata_node << YAML::Newline;

    metadata_yaml_file.open(QIODevice::WriteOnly);
    metadata_yaml_file.write(emitter.c_str());
    metadata_yaml_file.close();

    const auto metadata_tarball_path = lxd_import_dir.filePath("metadata.tar");
    auto process = MP_PROCFACTORY.create_process(
        "tar", QStringList() << "-cf" << metadata_tarball_path << "-C" << lxd_import_dir.path()
                             << QFileInfo(metadata_yaml_file.fileName()).fileName());

    auto exit_state = process->execute();

    if (!exit_state.completed_successfully())
    {
        throw std::runtime_error(
            fmt::format("Failed to create LXD image import metadata tarball: {}", process->read_all_standard_error()));
    }

    return metadata_tarball_path;
}
} // namespace

mp::LXDVMImageVault::LXDVMImageVault(std::vector<VMImageHost*> image_hosts, URLDownloader* downloader,
                                     NetworkAccessManager* manager, const QUrl& base_url, const QString& cache_dir_path,
                                     const days& days_to_expire)
    : image_hosts{image_hosts},
      url_downloader{downloader},
      manager{manager},
      base_url{base_url},
      template_path{QString("%1/%2-").arg(cache_dir_path).arg(QCoreApplication::applicationName())},
      days_to_expire{days_to_expire}
{
    for (const auto& image_host : image_hosts)
    {
        for (const auto& remote : image_host->supported_remotes())
        {
            if (mp::platform::is_remote_supported(remote))
            {
                remote_image_host_map[remote] = image_host;
            }
        }
    }
}

mp::VMImage mp::LXDVMImageVault::fetch_image(const FetchType& fetch_type, const Query& query,
                                             const PrepareAction& prepare, const ProgressMonitor& monitor)
{
    // Look for an already existing instance and get its image info
    try
    {
        auto instance_info = lxd_request(
            manager, "GET",
            QUrl(QString("%1/virtual-machines/%2").arg(base_url.toString()).arg(QString::fromStdString(query.name))));

        auto config = instance_info["metadata"].toObject()["config"].toObject();

        if (config.contains("image.original_hash"))
        {
            VMImage source_image;

            source_image.id = config["image.original_hash"].toString().toStdString();
            source_image.original_release = config["image.description"].toString().toStdString();
            source_image.release_date = config["image.version"].toString().toStdString();

            return source_image;
        }

        auto id = config["volatile.base_image"].toString();

        for (const auto& image_host : image_hosts)
        {
            try
            {
                auto info = image_host->info_for_full_hash(id.toStdString());

                VMImage source_image;

                source_image.id = id.toStdString();
                source_image.original_release = info.release_title.toStdString();
                source_image.release_date = info.version.toStdString();

                for (const auto& alias : info.aliases)
                {
                    source_image.aliases.push_back(alias.toStdString());
                }

                return source_image;
            }
            catch (const std::exception&)
            {
                // Ignore
            }
        }
    }
    catch (const LXDNotFoundException&)
    {
        // Instance doesn't exist, so move on
    }
    catch (const std::exception&)
    {
        // Image doesn't exist, so move on
    }

    // TODO: Remove once we do support http & file based images
    if (query.query_type != Query::Type::Alias)
        throw std::runtime_error("http and file based images are not supported");

    const auto info = info_for(query);
    const auto id = info.id;
    VMImage source_image;

    source_image.id = id.toStdString();
    source_image.original_release = info.release_title.toStdString();
    source_image.release_date = info.version.toStdString();

    for (const auto& alias : info.aliases)
    {
        source_image.aliases.push_back(alias.toStdString());
    }

    try
    {
        auto json_reply = lxd_request(manager, "GET", QUrl(QString("%1/images/%2").arg(base_url.toString()).arg(id)));
    }
    catch (const LXDNotFoundException&)
    {
        auto lxd_image_hash = get_lxd_image_hash_for(id);
        if (!lxd_image_hash.empty())
        {
            source_image.id = lxd_image_hash;
        }
        else if (!info.stream_location.isEmpty())
        {
            lxd_download_image(id, info.stream_location, QString::fromStdString(query.release), monitor);
        }
        else if (!info.image_location.isEmpty())
        {
            // TODO: Need to make this async like in DefaultVMImageVault
            QTemporaryDir lxd_import_dir{template_path};

            auto image_path = lxd_import_dir.filePath(mp::vault::filename_for(info.image_location));

            url_download_image(info, image_path, monitor);

            image_path = post_process_downloaded_image(image_path, monitor);

            monitor(LaunchProgress::WAITING, -1);

            auto metadata_tarball_path = create_metadata_tarball(info, lxd_import_dir);

            source_image.id = lxd_import_metadata_and_image(metadata_tarball_path, image_path);
        }
        else
        {
            throw std::runtime_error(fmt::format("Unable to fetch image with hash \'{}\'", id));
        }
    }

    return source_image;
}

void mp::LXDVMImageVault::remove(const std::string& name)
{
    try
    {
        lxd_request(manager, "DELETE",
                    QUrl(QString("%1/virtual-machines/%2").arg(base_url.toString()).arg(name.c_str())));
    }
    catch (const LXDNotFoundException&)
    {
        mpl::log(mpl::Level::warning, category, fmt::format("Instance \'{}\' does not exist: not removing", name));
    }
}

bool mp::LXDVMImageVault::has_record_for(const std::string& name)
{
    try
    {
        lxd_request(manager, "GET", QUrl(QString("%1/virtual-machines/%2").arg(base_url.toString()).arg(name.c_str())));

        return true;
    }
    catch (const LXDNotFoundException&)
    {
        return false;
    }
}

void mp::LXDVMImageVault::prune_expired_images()
{
    auto images = retrieve_image_list();

    for (const auto image : images)
    {
        auto image_info = image.toObject();
        auto last_used = std::chrono::system_clock::time_point(std::chrono::milliseconds(
            QDateTime::fromString(image_info["last_used_at"].toString(), Qt::ISODateWithMs).toMSecsSinceEpoch()));

        if (last_used + days_to_expire <= std::chrono::system_clock::now())
        {
            mpl::log(mpl::Level::info, category,
                     fmt::format("Source image \'{}\' is expired. Removing it…",
                                 image_info["properties"].toObject()["release"].toString()));

            try
            {
                lxd_request(
                    manager, "DELETE",
                    QUrl(QString("%1/images/%2").arg(base_url.toString()).arg(image_info["fingerprint"].toString())));
            }
            catch (const LXDNotFoundException&)
            {
                continue;
            }
        }
    }
}

void mp::LXDVMImageVault::update_images(const FetchType& fetch_type, const PrepareAction& prepare,
                                        const ProgressMonitor& monitor)
{
    auto images = retrieve_image_list();

    for (const auto image : images)
    {
        auto image_info = image.toObject();
        if (image_info.contains("update_source"))
        {
            auto release = image_info["properties"].toObject()["release"].toString();
            mpl::log(mpl::Level::debug, category, fmt::format("Checking if \'{}\' needs updating…", release));

            auto id = image_info["fingerprint"].toString();

            try
            {
                auto json_reply = lxd_request(manager, "POST",
                                              QUrl(QString("%1/images/%2/refresh").arg(base_url.toString()).arg(id)));

                auto task_complete = [&release](auto metadata) {
                    if (metadata["metadata"].toObject()["refreshed"].toBool())
                    {
                        mpl::log(mpl::Level::info, category, fmt::format("Image update for \'{}\' complete.", release));
                    }
                    else
                    {
                        mpl::log(mpl::Level::debug, category, fmt::format("No image update for \'{}\'.", release));
                    }
                };

                poll_download_operation(json_reply, monitor, task_complete);
            }
            catch (const LXDNotFoundException&)
            {
                continue;
            }
        }
    }
}

mp::VMImageInfo mp::LXDVMImageVault::info_for(const mp::Query& query)
{
    if (!query.remote_name.empty())
    {
        auto it = remote_image_host_map.find(query.remote_name);
        if (it == remote_image_host_map.end())
            throw std::runtime_error(fmt::format("Remote \"{}\" is unknown.", query.remote_name));

        auto info = it->second->info_for(query);

        if (info != nullopt)
            return *info;
    }
    else
    {
        for (const auto& image_host : image_hosts)
        {
            auto info = image_host->info_for(query);

            if (info)
            {
                return *info;
            }
        }
    }

    throw std::runtime_error(fmt::format("Unable to find an image matching \"{}\"", query.release));
}

void mp::LXDVMImageVault::lxd_download_image(const QString& id, const QString& stream_location, const QString& release,
                                             const ProgressMonitor& monitor)
{
    QJsonObject source_object;

    source_object.insert("type", "image");
    source_object.insert("mode", "pull");
    source_object.insert("server", stream_location);
    source_object.insert("protocol", "simplestreams");
    source_object.insert("image_type", "virtual-machine");

    if (id.startsWith(release))
    {
        source_object.insert("fingerprint", id);
    }
    else
    {
        source_object.insert("alias", release);
    }

    QJsonObject image_object{{"source", source_object}};

    auto json_reply = lxd_request(manager, "POST", QUrl(QString("%1/images").arg(base_url.toString())), image_object);

    poll_download_operation(json_reply, monitor);
}

void mp::LXDVMImageVault::url_download_image(const VMImageInfo& info, const QString& image_path,
                                             const ProgressMonitor& monitor)
{
    mp::vault::DeleteOnException image_file{image_path};

    url_downloader->download_to(info.image_location, image_path, info.size, LaunchProgress::IMAGE, monitor);

    if (info.verify)
    {
        monitor(LaunchProgress::VERIFY, -1);
        mp::vault::verify_image_download(image_path, info.id);
    }
}

void mp::LXDVMImageVault::poll_download_operation(const QJsonObject& json_reply, const ProgressMonitor& monitor,
                                                  const TaskCompleteAction& task_complete)
{
    if (json_reply["metadata"].toObject()["class"] == QStringLiteral("task") &&
        json_reply["status_code"].toInt(-1) == 100)
    {
        QUrl task_url(QString("%1/operations/%2")
                          .arg(base_url.toString())
                          .arg(json_reply["metadata"].toObject()["id"].toString()));

        // Instead of polling, need to use websockets to get events
        while (true)
        {
            try
            {
                auto task_reply = mp::lxd_request(manager, "GET", task_url);

                if (task_reply["error_code"].toInt(-1) != 0)
                {
                    mpl::log(mpl::Level::error, category, task_reply["error"].toString().toStdString());
                    break;
                }

                auto status_code = task_reply["metadata"].toObject()["status_code"].toInt(-1);
                if (status_code == 200)
                {
                    task_complete(task_reply["metadata"].toObject());
                    break;
                }
                else
                {
                    auto download_progress = parse_percent_as_int(
                        task_reply["metadata"].toObject()["metadata"].toObject()["download_progress"].toString());

                    if (!monitor(LaunchProgress::IMAGE, download_progress))
                    {
                        mp::lxd_request(manager, "DELETE", task_url);
                        throw mp::AbortedDownloadException{"Download aborted"};
                    }

                    std::this_thread::sleep_for(1s);
                }
            }
            // Implies the task is finished
            catch (const LXDNotFoundException&)
            {
                break;
            }
        }
    }
}

std::string mp::LXDVMImageVault::lxd_import_metadata_and_image(const QString& metadata_path, const QString& image_path)
{
    QHttpMultiPart lxd_multipart{QHttpMultiPart::FormDataType};
    QFileInfo metadata_info{metadata_path}, image_info{image_path};

    QHttpPart metadata_part;
    metadata_part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
    metadata_part.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QString("form-data; name=\"metadata\"; filename=\"%1\"").arg(metadata_info.fileName())));
    QFile* metadata_file = new QFile(metadata_path);
    metadata_file->open(QIODevice::ReadOnly);
    metadata_part.setBodyDevice(metadata_file);
    metadata_file->setParent(&lxd_multipart);

    QHttpPart image_part;
    image_part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
    image_part.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QVariant(QString("form-data; name=\"rootfs.img\"; filename=\"%1\"").arg(image_info.fileName())));
    QFile* image_file = new QFile(image_path);
    image_file->open(QIODevice::ReadOnly);
    image_part.setBodyDevice(image_file);
    image_file->setParent(&lxd_multipart);

    lxd_multipart.append(metadata_part);
    lxd_multipart.append(image_part);

    auto json_reply = lxd_request(manager, "POST", QUrl(QString("%1/images").arg(base_url.toString())), lxd_multipart);

    if (json_reply["metadata"].toObject()["class"] == QStringLiteral("task") &&
        json_reply["status_code"].toInt(-1) == 100)
    {
        QUrl task_url(QString("%1/operations/%2/wait")
                          .arg(base_url.toString())
                          .arg(json_reply["metadata"].toObject()["id"].toString()));

        auto task_reply = lxd_request(manager, "GET", task_url, mp::nullopt, 300000);

        return task_reply["metadata"].toObject()["metadata"].toObject()["fingerprint"].toString().toStdString();
    }

    throw std::runtime_error("Unable to retrieve hash for image from LXD");
}

std::string mp::LXDVMImageVault::get_lxd_image_hash_for(const QString& id)
{
    auto images = retrieve_image_list();

    for (const auto image : images)
    {
        auto image_info = image.toObject();
        auto properties = image_info["properties"].toObject();

        if (properties.contains("original_hash"))
        {
            auto original_hash = properties["original_hash"].toString();
            if (original_hash == id)
            {
                return image_info["fingerprint"].toString().toStdString();
            }
        }
    }

    return {};
}

QJsonArray mp::LXDVMImageVault::retrieve_image_list()
{
    QJsonArray image_list;

    try
    {
        auto json_reply = lxd_request(manager, "GET", QUrl(QString("%1/images?recursion=1").arg(base_url.toString())));

        image_list = json_reply["metadata"].toArray();
    }
    catch (const LXDNotFoundException&)
    {
        // ignore exception
    }

    return image_list;
}
