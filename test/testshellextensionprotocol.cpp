/*
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QtTest>

#include "libsync/common/shellextensionutils.h"

using namespace VfsShellExtensions;

class TestShellExtensionProtocol : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRoundTrip()
    {
        const QByteArray png(300 * 1024, 'x');
        const auto data = Protocol::createMessage({{QString::fromLatin1(Protocol::ThumbnailProviderDataKey), QString::fromLatin1(png.toBase64())}});

        Protocol::MessageReader reader;
        reader.append(data);
        const auto message = reader.takeMessage();
        QVERIFY(message.has_value());
        QVERIFY(Protocol::validateProtocolVersion(*message));
        QCOMPARE(QByteArray::fromBase64(message->value(QString::fromLatin1(Protocol::ThumbnailProviderDataKey)).toByteArray()), png);
        // nothing left
        QVERIFY(!reader.takeMessage().has_value());
        QVERIFY(!reader.hasError());
    }

    void testPartialReads()
    {
        const auto data = Protocol::createMessage({{QString::fromLatin1(Protocol::FilePathKey), QStringLiteral("C:/Users/me/Pictures/a.jpg")}});
        Protocol::MessageReader reader;
        // a message that arrives in pieces is only taken once it is complete
        for (qsizetype i = 0; i < data.size() - 1; ++i) {
            reader.append(data.mid(i, 1));
            QVERIFY(!reader.takeMessage().has_value());
        }
        reader.append(data.right(1));
        const auto message = reader.takeMessage();
        QVERIFY(message.has_value());
        QCOMPARE(message->value(QString::fromLatin1(Protocol::FilePathKey)).toString(), QStringLiteral("C:/Users/me/Pictures/a.jpg"));
    }

    void testInvalidInput()
    {
        {
            // a length above the limit is rejected without waiting for the data
            Protocol::MessageReader reader;
            reader.append(QByteArray::fromHex("7fffffff"));
            QVERIFY(!reader.takeMessage().has_value());
            QVERIFY(reader.hasError());
        }
        {
            Protocol::MessageReader reader;
            reader.append(QByteArray::fromHex("00000003") + QByteArrayLiteral("abc"));
            QVERIFY(!reader.takeMessage().has_value());
            QVERIFY(reader.hasError());
        }
        // a message without version is from an incompatible peer
        QVERIFY(!Protocol::validateProtocolVersion({{QStringLiteral("thumbnailData"), QString()}}));
    }

    void testServerName()
    {
        const auto name = serverNameForApplicationName(QStringLiteral("OpenCloud"));
        QCOMPARE(name, serverNameForApplicationName(QStringLiteral("OpenCloud")));
        QVERIFY(name.startsWith(QStringLiteral("OpenCloud-VfsShellExtensions-")));
        QVERIFY(name != serverNameForApplicationName(QStringLiteral("OpenCloud Beta")));
    }
};

QTEST_GUILESS_MAIN(TestShellExtensionProtocol)
#include "testshellextensionprotocol.moc"
