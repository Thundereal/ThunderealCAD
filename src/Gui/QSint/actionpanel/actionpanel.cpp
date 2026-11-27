/***************************************************************************
 *                                                                         *
 *   Copyright: https://code.google.com/p/qsint/                           *
 *   License:   LGPL                                                       *
 *                                                                         *
 ***************************************************************************/

#include "actionpanel.h"
#include "actionpanelscheme.h"
#include "actiongroup.h"

#include <QVariant>


namespace QSint
{


ActionPanel::ActionPanel(QWidget *parent) :
    BaseClass(parent), mySpacer(nullptr)
{
    setProperty("class", "panel");

    setScheme(ActionPanelScheme::defaultScheme());

    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    QVBoxLayout *vbl = new QVBoxLayout();
    vbl->setContentsMargins(8, 8, 8, 8);
    vbl->setSpacing(8);
    setLayout(vbl);
}

void ActionPanel::setScheme(ActionPanelScheme *scheme)
{
  if (scheme) {
    myScheme = scheme;
    // Only a sheet that differs: setStyleSheet() repolishes the whole
    // subtree even when the sheet is the one it has, and the task view
    // asks again at every edit's start and end -- some 3300 style
    // changes each time, which an application style sheet makes cost
    // a second and more (leaving a sketch: 1.6 s against 0.3).
    if (styleSheet() != myScheme->actionStyle)
        setStyleSheet(myScheme->actionStyle);

    // set scheme for children
    QObjectList list(children());
    Q_FOREACH(QObject *obj, list) {
      if (dynamic_cast<ActionGroup*>(obj)) {
        ((ActionGroup*)obj)->setScheme(scheme);
        continue;
      }
    }

    update();
  }
}

//void ActionPanel::paintEvent ( QPaintEvent * event )
//{
//  //QPainter p(this);

//  //p.setOpacity(0.5);
//  //p.fillRect(rect(), myScheme->panelBackground);
//}

void ActionPanel::addWidget(QWidget *w)
{
  if (w)
    layout()->addWidget(w);
}

void ActionPanel::removeWidget(QWidget *w)
{
  if (w)
    layout()->removeWidget(w);
}

void ActionPanel::addStretch(int s)
{
  Q_UNUSED(s); 
  //((QVBoxLayout*)layout())->addStretch(s);
  if (!mySpacer) {
    mySpacer = new QSpacerItem(0,0,QSizePolicy::Minimum, QSizePolicy::Expanding);
    layout()->addItem(mySpacer);
  }
}

void ActionPanel::removeStretch()
{
  if (mySpacer) {
    layout()->removeItem(mySpacer);
    delete mySpacer;
    mySpacer = nullptr;
  }
}

ActionGroup * ActionPanel::createGroup()
{
    ActionGroup * group = new ActionGroup(this);
    addWidget(group);
    return group;
}

ActionGroup * ActionPanel::createGroup(const QString &title, bool expandable)
{
    ActionGroup * box = new ActionGroup(title, expandable, this);
    addWidget(box);
    return box;
}

ActionGroup * ActionPanel::createGroup(const QPixmap &icon, const QString &title, bool expandable)
{
    ActionGroup * box = new ActionGroup(icon, title, expandable, this);
    addWidget(box);
    return box;
}


QSize ActionPanel::minimumSizeHint() const
{
    return {200,150};
}


} // namespace
