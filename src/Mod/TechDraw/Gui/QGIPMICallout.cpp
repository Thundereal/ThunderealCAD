// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2026 Zheng Lei (realthunder) <realthunder.dev@gmail.com> *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QPen>

#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>

#include <Mod/TechDraw/App/DrawPMICallout.h>
#ifdef FC_TECHDRAW_PMI
#include <Mod/PMI/App/Callout.h>
#include <Mod/PMI/App/Drawing.h>
#endif

#include "QGIPMICallout.h"
#include "Rez.h"
#include "ViewProviderBalloon.h"

using namespace TechDrawGui;

QGIPMICallout::QGIPMICallout()
{
    // Children of the label: they move with it while it is dragged, and
    // are what it is picked by
    m_strokes = new QGraphicsPathItem(getBalloonLabel());
    m_fills = new QGraphicsPathItem(getBalloonLabel());
    m_fills->setPen(Qt::NoPen);
}

bool QGIPMICallout::drawSymbol(double& halfWidth, double& halfHeight)
{
    auto feat = dynamic_cast<TechDraw::DrawPMICallout*>(getBalloonFeat());
    if (!feat) {
        return false;
    }
    QGIBalloonLabel* label = getBalloonLabel();
    // The balloon's own text is not shown: the frame says it
    label->getDimText()->setVisible(false);
    QPainterPath strokes;
    QPainterPath fills;
    fills.setFillRule(Qt::OddEvenFill);
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
#ifdef FC_TECHDRAW_PMI
    auto callout = dynamic_cast<PMI::Callout*>(feat->Source.getValue());
    auto vp = dynamic_cast<ViewProviderBalloon*>(getViewProvider(feat));
    if (callout && vp) {
        // At the balloon's lettering height, the paper's; y up in the
        // drawing, down on the page
        PMI::Drawing d;
        callout->layout(d, vp->Fontsize.getValue());
        if (!d.isEmpty()) {
            x0 = d.xmin;
            y0 = d.ymin;
            x1 = d.xmax;
            y1 = d.ymax;
            double cx = (x0 + x1) / 2;
            double cy = (y0 + y1) / 2;
            auto at = [cx, cy](const TopoDS_Vertex& v) {
                gp_Pnt p = BRep_Tool::Pnt(v);
                return QPointF(Rez::guiX(p.X() - cx), -Rez::guiX(p.Y() - cy));
            };
            TopoDS_Shape shape = d.shape();
            for (TopExp_Explorer it(shape, TopAbs_EDGE, TopAbs_FACE); it.More(); it.Next()) {
                const TopoDS_Edge& e = TopoDS::Edge(it.Current());
                strokes.moveTo(at(TopExp::FirstVertex(e, true)));
                strokes.lineTo(at(TopExp::LastVertex(e, true)));
            }
            for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
                for (TopExp_Explorer w(f.Current(), TopAbs_WIRE); w.More(); w.Next()) {
                    bool first = true;
                    for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current())); e.More();
                         e.Next()) {
                        QPointF p = at(e.CurrentVertex());
                        if (first) {
                            fills.moveTo(p);
                            first = false;
                        }
                        else {
                            fills.lineTo(p);
                        }
                    }
                    fills.closeSubpath();
                }
            }
        }
    }
#endif
    // About the label's centre, which is its text box's
    QRectF box = label->getDimText()->boundingRect();
    QPointF centre(box.width() / 2, box.height() / 2);
    m_strokes->setPos(centre);
    m_fills->setPos(centre);
    m_strokes->setPath(strokes);
    m_fills->setPath(fills);
    QColor color = prefNormalColor();
    QPen pen(color);
    auto vpBalloon = dynamic_cast<ViewProviderBalloon*>(getViewProvider(feat));
    pen.setWidthF(vpBalloon ? Rez::guiX(vpBalloon->LineWidth.getValue()) : Rez::guiX(0.35));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    m_strokes->setPen(pen);
    m_fills->setBrush(color);
    halfWidth = Rez::guiX(x1 - x0) / 2;
    halfHeight = Rez::guiX(y1 - y0) / 2;
    return true;
}
