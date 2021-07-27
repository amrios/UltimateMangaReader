#include "mangadex.h"

using namespace rapidjson;

MangaDex::MangaDex(NetworkManager *dm) : AbstractMangaSource(dm)
{
    name = "MangaDex";
    baseUrl = "https://api.mangadex.org";
    basedictUrl = baseUrl + "/manga?limit=100&offset=";

    networkManager->addCookie(".mangadex.org", "mangadex_h_toggle", "1");
    networkManager->addCookie(".mangadex.org", "mangadex_title_mode", "2");
    networkManager->addCookie(".mangadex.org", "mangadex_filter_langs", "1");

    statuses = {"Ongoing", "Completed", "Cancelled", "Hiatus"};
    demographies = {"Shounen", "Shoujo", "Seinen", "Josei"};
    genreMap.insert(2, "Action");
    genreMap.insert(3, "Adventure");
    genreMap.insert(5, "Comedy");
    genreMap.insert(8, "Drama");
    genreMap.insert(9, "Ecchi");
    genreMap.insert(10, "Fantasy");
    genreMap.insert(13, "Historical");
    genreMap.insert(14, "Horror");
    genreMap.insert(17, "Mecha");
    genreMap.insert(18, "Medical");
    genreMap.insert(20, "Mystery");
    genreMap.insert(22, "Psychological");
    genreMap.insert(23, "Romance");
    genreMap.insert(25, "Sci-Fi");
    genreMap.insert(28, "Shoujo Ai");
    genreMap.insert(30, "Shounen Ai");
    genreMap.insert(31, "Slice of Life");
    genreMap.insert(32, "Smut");
    genreMap.insert(33, "Sports");
    genreMap.insert(35, "Tragedy");
    genreMap.insert(37, "Yaoi");
    genreMap.insert(38, "Yuri");
    genreMap.insert(41, "Isekai");
    genreMap.insert(49, "Gore");
    genreMap.insert(50, "Sexual Violence");
    genreMap.insert(51, "Crime");
    genreMap.insert(52, "Magical Girls");
    genreMap.insert(53, "Philosophical");
    genreMap.insert(54, "Superhero");
    genreMap.insert(55, "Thriller");
    genreMap.insert(56, "Wuxia");
}

bool MangaDex::updateMangaList(UpdateProgressToken *token)
{
    MangaList mangas;

    token->sendProgress(1);

    QElapsedTimer timer;
    timer.start();

    try
    {
        Document doc;
        // Mangadex limits results to 10000, so a workaround is required.
        // Our partial workaround allows us to get more entries by using status filters.
        for (const auto &s : statuses) {
            auto statusFilter = QString("&status[]=" + s.toLower());
            bool isAtEnd = false;

            for (unsigned int i = 0; i < 10000 && !isAtEnd; i = i + 100) {
                auto currentOffset = QString::number(i);

                auto job = networkManager->downloadAsString(basedictUrl + currentOffset + statusFilter, -1);

                if (!job->await(7000))
                {
                    token->sendError(job->errorString);
                    return false;
                }

                ParseResult res = doc.Parse(job->buffer.data());
                if (!res)
                    return false;

                if (doc.HasMember("result") && QString(doc["result"].GetString()) == "error")
                    return false;
                auto results = doc["results"].GetArray();
                for (const auto &r : results)
                {
                    QString title;
                    if (r["data"]["attributes"]["title"].HasMember("en"))
                        title = QString(r["data"]["attributes"]["title"]["en"].GetString());
                    else
                        title = QString(r["data"]["attributes"]["title"]["jp"].GetString());
                    auto url = QString("/manga/%1?%2").arg(r["data"]["id"].GetString()).arg("includes[]=author&includes[]=artist&includes[]=cover_art");
                    mangas.append(title, url);
                    token->sendProgress(i % 10000 / 100);
                }

                unsigned int total = doc["total"].GetInt();
                if (i >= total-100)
                    isAtEnd = true;
            }
        }

    }
    catch (QException &)
    {
        return false;
    }

    this->mangaList = mangas;

    qDebug() << "mangas:" << mangas.size << "time:" << timer.elapsed();

    token->sendProgress(100);

    return true;
}

QString padChapterNumber(const QString &number, int places = 4)
{
    auto range = number.split('-');
    QStringList result;
    std::transform(range.begin(), range.end(), std::back_inserter(result), [places](QString chapter) {
        chapter = chapter.trimmed();
        auto digits = chapter.split('.')[0].length();
        return QString("0").repeated(qMax(0, places - digits)) + chapter;
    });
    return result.join('-');
}

Result<MangaChapterCollection, QString> MangaDex::updateMangaInfoFinishedLoading(
    QSharedPointer<DownloadStringJob> job, QSharedPointer<MangaInfo> info)
{
    //    QElapsedTimer t;
    //    t.start();
    QRegularExpression bbrx(R"(\[.*?\])");

    MangaChapterCollection newchapters;
    try
    {
        Document doc;

        ParseResult res = doc.Parse(job->buffer.data());
        if (!res)
            return Err(QString("Coulnd't parse mangainfos.1"));

        auto &mangaObject = doc["data"]["attributes"];

        //        info->author = htmlToPlainText(QString(mangaObject["author"].GetString()));
        //        info->artist = htmlToPlainText(QString(mangaObject["artist"].GetString()));

        if (mangaObject.HasMember("status") && !mangaObject["status"].IsNull())
            info->status = QString(mangaObject["status"].GetString());

        if (mangaObject.HasMember("year") && !mangaObject["year"].IsNull())
            info->releaseYear = QString(mangaObject["year"].GetString());

        if (mangaObject.HasMember("publicationDemographic") && !mangaObject["publicationDemographic"].IsNull())
            info->genres = QString(mangaObject["publicationDemographic"].GetString());

        info->summary = htmlToPlainText(QString(mangaObject["description"]["en"].GetString())).remove(bbrx);

        //        "https://uploads.mangadex.org/covers/{}/{}.256.jpg"

        auto rels = doc["relationships"].GetArray();

        for (const auto &rel : rels)
        {
            auto relation = QString(rel["type"].GetString());

            if (relation.compare(QString("author")) == 0)
                info->author = QString(rel["attributes"]["name"].GetString());
            if (relation.compare(QString("artist")) == 0)
                info->artist = QString(rel["attributes"]["name"].GetString());
            if (relation.compare(QString("cover_art")) == 0)
            {
                info->coverUrl = QString("https://uploads.mangadex.org/covers/%1/%2.256.jpg")
                                     .arg(doc["data"]["id"].GetString(),
                                          rel["attributes"]["fileName"].GetString());
            }

        }

        Document chDoc;
        auto id = QString(doc["data"]["id"].GetString());
        auto jobChapter = networkManager->downloadAsString("https://api.mangadex.org/chapter?manga=" + id + "&limit=100", -1);
        if (!jobChapter->await(3000))
        {
            return Err(jobChapter->errorString);
        }

        chDoc.Parse(jobChapter->buffer.data());
        unsigned int total = chDoc["total"].GetInt();

        for (unsigned int i = 0; i < total; i = i + 100)
        {
            QString offset = QString::number(i);
            jobChapter = networkManager->downloadAsString("https://api.mangadex.org/chapter?manga=" + id + "&limit=100&offset=" + offset, -1);
            if (!jobChapter->await(3000))
            {
                return Err(jobChapter->errorString);
            }

            chDoc.Parse(jobChapter->buffer.data());
            auto chaptersArr = chDoc["results"].GetArray();
            for (const auto &c : chaptersArr)
            {
                auto language = QString(c["data"]["attributes"]["translatedLanguage"].GetString());

                if (language != "en")
                    continue;

                auto title = QString("");
                auto numChapter = QString("");

                if (!c["data"]["attributes"]["title"].IsNull() && !c["data"]["attributes"]["chapter"].IsNull())
                {
                    title = QString(c["data"]["attributes"]["title"].GetString());
                    numChapter = QString(c["data"]["attributes"]["chapter"].GetString());
                }

                auto chapterTitle = "Ch. " + numChapter + " " + title;

                auto chapterKey = QString(c["data"]["id"].GetString());
                auto chapterUrl = QString("https://api.mangadex.org/chapter/" + chapterKey);

                MangaChapter mangaChapter(chapterTitle, chapterUrl);
                mangaChapter.chapterNumber = padChapterNumber(numChapter);

                newchapters.append(mangaChapter);
            }
        }


        //        auto &chaptersObject = doc["chapter"];

        //        for (auto it = chaptersObject.MemberBegin(); it != chaptersObject.MemberEnd(); ++it)
        //        {
        //            auto chapterKey = it->name.GetString();
        //            auto &chapterObject = it->value;
        //            auto language = QString(chapterObject["lang_code"].GetString());

        //            if (language != "gb")
        //                continue;

        //            auto numChapter = QString(chapterObject["chapter"].GetString());

        //            auto chapterTitle = "Ch. " + numChapter + " " +
        //            QString(chapterObject["title"].GetString());

        //            auto chapterUrl = QString("https://mangadex.org/api/?type=chapter&id=") + chapterKey;

        //            MangaChapter mangaChapter(chapterTitle, chapterUrl);
        //            mangaChapter.chapterNumber = padChapterNumber(numChapter);

        //            newchapters.insert(0, mangaChapter);
        //        }
    }
    catch (QException &)
    {
        return Err(QString("Coulnd't parse mangainfos.2"));
    }

    return Ok(newchapters);
}

Result<QStringList, QString> MangaDex::getPageList(const QString &chapterUrl)
{
    auto job = networkManager->downloadAsString(chapterUrl);

    if (!job->await(7000))
        return Err(job->errorString);

    QStringList imageUrls;

    try
    {
        Document doc;
        ParseResult res = doc.Parse(job->buffer.data());
        if (!res)
            return Err(QString("Coulnd't parse pagelist.1"));

        auto hash = QString(doc["data"]["attributes"]["hash"].GetString());
        auto server = QString("https://uploads.mangadex.org/data/");
        auto pagesArray = doc["data"]["attributes"]["data"].GetArray();
        for (const auto &page : pagesArray)
            imageUrls.append(server + hash + "/" + page.GetString());
    }
    catch (QException &)
    {
        return Err(QString("Coulnd't parse pagelist.2"));
    }

    return Ok(imageUrls);
}
