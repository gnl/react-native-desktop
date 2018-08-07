
/**
 * Copyright (C) 2016, Canonical Ltd.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 * Author: Justin McPherson <justin.mcpherson@canonical.com>
 *
 */

#include <QQmlComponent>
#include <QQuickItem>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <QDebug>

#include "attachedproperties.h"
#include "bridge.h"
#include "layout/flexbox.h"
#include "propertyhandler.h"
#include "reactitem.h"
#include "scrollviewmanager.h"
#include "uimanager.h"
#include "utilities.h"

using namespace utilities;

QMap<QQuickItem*, QQuickItem*> ScrollViewManager::m_scrollViewByListViewItem;
QMap<QQuickItem*, QVariantList> ScrollViewManager::m_modelByScrollView;

void ScrollViewManager::scrollTo(int reactTag, double offsetX, double offsetY, bool animated) {
    QQuickItem* item = bridge()->uiManager()->viewForTag(reactTag);
    Q_ASSERT(item != nullptr);

    QQmlProperty(item, "contentX").write(offsetX);
    QQmlProperty(item, "contentY").write(offsetY);
}

void ScrollViewManager::scrollToEnd(int reactTag, bool animated) {
    QQuickItem* item = bridge()->uiManager()->viewForTag(reactTag);
    Q_ASSERT(item != nullptr);

    if (arrayScrollingOptimizationEnabled(item)) {
        int scrollViewModelItemsCount = item->property("count").toInt();
        QMetaObject::invokeMethod(item, "positionViewAtEnd");
        QQmlProperty(item, "currentIndex").write(scrollViewModelItemsCount - 1);
        QMetaObject::invokeMethod(item, "positionViewAtEnd");
    } else {
        qreal contentHeight = item->property("contentHeight").toReal();
        qreal height = item->property("height").toReal();
        qreal newContentY = (contentHeight > height) ? contentHeight - height : 0;
        QQmlProperty(item, "contentY").write(newContentY);
    }
}

ScrollViewManager::ScrollViewManager(QObject* parent) : ViewManager(parent) {}

ScrollViewManager::~ScrollViewManager() {}

ViewManager* ScrollViewManager::viewManager() {
    return this;
}

QString ScrollViewManager::moduleName() {
    return "RCTScrollViewManager";
}

QStringList ScrollViewManager::customDirectEventTypes() {
    return QStringList{"scrollBeginDrag",
                       normalizeInputEventName("onScroll"),
                       "scrollEndDrag",
                       "scrollAnimationEnd",
                       "momentumScrollBegin",
                       "momentumScrollEnd"};
}

bool ScrollViewManager::isArrayScrollingOptimizationEnabled(QQuickItem* item) {
    return m_scrollViewByListViewItem.contains(item);
}

void ScrollViewManager::updateListViewItem(QQuickItem* item, QQuickItem* child, int position) {
    QQuickItem* scrollView = m_scrollViewByListViewItem[item];
    QVariantList& variantList = m_modelByScrollView[scrollView];
    variantList.insert(position, QVariant::fromValue(child));
    QQmlProperty::write(scrollView, "model", QVariant::fromValue(variantList));
}

void ScrollViewManager::removeListViewItem(QQuickItem* item,
                                           const QList<int>& removeAtIndices,
                                           bool unregisterAndDelete) {
    if (removeAtIndices.isEmpty())
        return;

    QQuickItem* scrollView = m_scrollViewByListViewItem[item];
    QVariantList& variantList = m_modelByScrollView[scrollView];

    foreach (int idxToRemote, removeAtIndices) {
        QQuickItem* itemToRemove = variantList.takeAt(idxToRemote).value<QQuickItem*>();
        itemToRemove->setParentItem(nullptr);

        if (unregisterAndDelete) {
            itemToRemove->setParent(0);
            itemToRemove->deleteLater();
        }
    }

    auto flexbox = Flexbox::findFlexbox(item);
    if (flexbox) {
        flexbox->removeChilds(removeAtIndices);
    }

    QQmlProperty::write(scrollView, "model", QVariant::fromValue(variantList));
}

QQuickItem* ScrollViewManager::scrollViewContentItem(QQuickItem* item, int position) {
    QQuickItem* scrollView = m_scrollViewByListViewItem[item];
    QVariantList& variantList = m_modelByScrollView[scrollView];

    Q_ASSERT(position < variantList.size());
    return variantList.takeAt(position).value<QQuickItem*>();
}

void ScrollViewManager::addChildItem(QQuickItem* scrollView, QQuickItem* child, int position) const {
    if (arrayScrollingOptimizationEnabled(scrollView)) {
        QVariantList& list = m_modelByScrollView[scrollView];
        foreach (QQuickItem* item, child->childItems()) { list.append(QVariant::fromValue(item)); }
        QQmlProperty::write(scrollView, "model", QVariant::fromValue(list));
        m_scrollViewByListViewItem.insert(child, scrollView);
    } else {
        // Flickable items should be children of contentItem
        QQuickItem* contentItem = QQmlProperty(scrollView, "contentItem").read().value<QQuickItem*>();
        Q_ASSERT(contentItem != nullptr);
        utilities::insertChildItemAt(child, position, contentItem);
    }
}

void ScrollViewManager::scrollBeginDrag() {
    // qDebug() << __PRETTY_FUNCTION__;
    QQuickItem* item = qobject_cast<QQuickItem*>(sender());
    Q_ASSERT(item != nullptr);
    notifyJsAboutEvent(tag(item), "scrollBeginDrag", {});
}

void ScrollViewManager::scrollEndDrag() {
    // qDebug() << __PRETTY_FUNCTION__;
    QQuickItem* item = qobject_cast<QQuickItem*>(sender());
    Q_ASSERT(item != nullptr);
    notifyJsAboutEvent(tag(item), "scrollEndDrag", {});
}

void ScrollViewManager::scroll() {
    QQuickItem* item = qobject_cast<QQuickItem*>(sender());
    Q_ASSERT(item != nullptr);

    bool scrollFlagSet = item->property("p_onScroll").toBool();

    if (scrollFlagSet) {
        notifyJsAboutEvent(tag(item), "onScroll", buildEventData(item));
    }
}

void ScrollViewManager::momentumScrollBegin() {
    // qDebug() << __PRETTY_FUNCTION__;
    QQuickItem* item = qobject_cast<QQuickItem*>(sender());
    Q_ASSERT(item != nullptr);
    notifyJsAboutEvent(tag(item), "momentumScrollBegin", buildEventData(item));
}

void ScrollViewManager::momentumScrollEnd() {
    // qDebug() << __PRETTY_FUNCTION__;
    QQuickItem* item = qobject_cast<QQuickItem*>(sender());
    Q_ASSERT(item != nullptr);
    notifyJsAboutEvent(tag(item), "momentumScrollEnd", buildEventData(item));
}

namespace {
template <typename TP> TP propertyValue(QQuickItem* item, const QString& property) {
    return QQmlProperty(item, property).read().value<TP>();
}
} // namespace

QVariantMap ScrollViewManager::buildEventData(QQuickItem* item) const {
    QVariantMap ed;
    ed.insert("contentOffset",
              QVariantMap{
                  {"x", propertyValue<double>(item, "contentX") - propertyValue<double>(item, "originX")},
                  {"y", propertyValue<double>(item, "contentY") - propertyValue<double>(item, "originY")},
              });
    // ed.insert("contentInset", QVariantMap{
    // });
    ed.insert("contentSize",
              QVariantMap{
                  {"width", propertyValue<double>(item, "contentWidth")},
                  {"height", propertyValue<double>(item, "contentHeight")},
              });
    ed.insert("layoutMeasurement",
              QVariantMap{
                  {"width", propertyValue<double>(item, "width")}, {"height", propertyValue<double>(item, "height")},

              });
    ed.insert("zoomScale", 1);
    return ed;
}

void ScrollViewManager::configureView(QQuickItem* view) const {
    ViewManager::configureView(view);
    view->setProperty("scrollViewManager", QVariant::fromValue((QObject*)this));
    // This would be prettier with a Functor version, but connect doesnt support it
    connect(view, SIGNAL(movementStarted()), SLOT(scrollBeginDrag()));
    connect(view, SIGNAL(movementEnded()), SLOT(scrollEndDrag()));
    connect(view, SIGNAL(movingChanged()), SLOT(scroll()));

    connect(view, SIGNAL(flickStarted()), SLOT(momentumScrollBegin()));
    connect(view, SIGNAL(flickEnded()), SLOT(momentumScrollEnd()));
}

QString ScrollViewManager::qmlComponentFile(const QVariantMap& properties) const {

    return properties.value("enableArrayScrollingOptimization", false).toBool() ? "qrc:/qml/ReactScrollListView.qml"
                                                                                : "qrc:/qml/ReactScrollView.qml";
}

bool ScrollViewManager::arrayScrollingOptimizationEnabled(QQuickItem* item) const {
    return QQmlProperty(item, "p_enableArrayScrollingOptimization").read().toBool();
}

#include "scrollviewmanager.moc"
