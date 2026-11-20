/***************************************************************************
 *   Copyright (c) 2011 Luke Parry                                         *
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

#ifndef _PreComp_
# include <Inventor/actions/SoGetMatrixAction.h>
# include <Inventor/actions/SoGLRenderAction.h>
# include <Inventor/actions/SoCallbackAction.h>
# include <Inventor/actions/SoPickAction.h>
# include <Inventor/actions/SoGetBoundingBoxAction.h>
# include <Inventor/actions/SoGetPrimitiveCountAction.h>
# include <Inventor/elements/SoModelMatrixElement.h>
# include <Inventor/elements/SoViewingMatrixElement.h>
# include <Inventor/elements/SoViewportRegionElement.h>
# include <Inventor/elements/SoViewVolumeElement.h>
# include <Inventor/nodes/SoCamera.h>
#endif

#include "SoAutoZoomTranslation.h"


using namespace Gui;

// ------------------------------------------------------

SO_NODE_SOURCE(SoAutoZoomTranslation)

void SoAutoZoomTranslation::initClass()
{
    SO_NODE_INIT_CLASS(SoAutoZoomTranslation, SoTransformation, "AutoZoom");

    // Enable elements for SoGetMatrixAction (#0002268)
    // SoCamera::initClass() enables the SoViewVolumeElement for
    // * SoGLRenderAction
    // * SoGetBoundingBoxAction
    // * SoRayPickAction
    // * SoCallbackAction
    // * SoGetPrimitiveCountAction
    // The element SoViewportRegionElement is enabled by the
    // above listed actions.
    // Additionally, SoViewVolumeElement is enabled for
    // * SoAudioRenderAction
    // * SoHandleEventAction
    // And SoViewportRegionElement is enabled for
    // * SoHandleEventAction
    // * SoGetMatrixAction
    SO_ENABLE(SoGetMatrixAction, SoViewVolumeElement);
}

float SoAutoZoomTranslation::getScaleFactor(SoAction* action) const
{
    float scale = scaleFactor.getValue();
    if(!scale)
        return 1.0;
    // Dividing by 5 seems to work well
    SbViewVolume vv = SoViewVolumeElement::get(action->getState());
    float aspectRatio = SoViewportRegionElement::get(action->getState()).getViewportAspectRatio();
    scale *= vv.getWorldToScreenScale(SbVec3f(0.f, 0.f, 0.f), 0.1f) / (5*aspectRatio);
    return scale;
}

SoAutoZoomTranslation::SoAutoZoomTranslation()
{
    SO_NODE_CONSTRUCTOR(SoAutoZoomTranslation);
    SO_NODE_ADD_FIELD(scaleFactor, (1.0f));
    SO_NODE_ADD_FIELD(billboard, (FALSE));
    SO_NODE_ADD_FIELD(pixelScale, (0.0f));
    SO_NODE_ADD_FIELD(datumFlip, (FALSE));
    SO_NODE_ADD_FIELD(flipNormal, (SbVec3f(0.f, 0.f, 1.f)));
    //SO_NODE_ADD_FIELD(abPos, (SbVec3f(0.f,0.f,0.f)));
}

void SoAutoZoomTranslation::GLRender(SoGLRenderAction * action)
{
    SoAutoZoomTranslation::doAction(static_cast<SoAction *>(action));
    inherited::GLRender(action);
}

// Doc in superclass.
void SoAutoZoomTranslation::doAction(SoAction * action)
{
    float sf = this->getScaleFactor(action);
    auto state = action->getState();
    SbRotation r,so;
    SbVec3f s,t;
    SbMatrix matrix = SoModelMatrixElement::get(action->getState()); // clazy:exclude=rule-of-two-soft
    matrix.getTransform(t,r,s,so);
    matrix.multVecMatrix(SbVec3f(0,0,0),t);
    // reset current model scale factor
    matrix.setTransform(t,r,SbVec3f(sf,sf,sf));
    if (datumFlip.getValue()
        && state->isElementEnabled(SoViewingMatrixElement::getClassStackIndex())) {
        flip(matrix, SoViewingMatrixElement::get(state));
    }
    SoModelMatrixElement::set(state,this,matrix);
}

void SoAutoZoomTranslation::flip(SbMatrix& m, const SbMatrix& v) const
{
    // The backend's per-frame test (BGFXRendererP.h, datumFlip), so Coin, the
    // GL render cache that replays this node and a pick all agree: project
    // the local X and Y (rows 0 and 1, row vectors) onto the view's x and y,
    // and mirror whichever points left or down. Backfacing settles the cases
    // the projection leaves in doubt, edge on.
    const float projX = m[0][0]*v[0][0] + m[0][1]*v[1][0] + m[0][2]*v[2][0];
    const float projY = m[1][0]*v[0][1] + m[1][1]*v[1][1] + m[1][2]*v[2][1];
    const float det = m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])
                    - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
                    + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    const float margin = det < 0.f ? -1e-3f : 1e-3f;
    float xfactor = (-2.0f*projX < margin) ? 0.5f : -0.5f;
    float yfactor = (-2.0f*projY < margin) ? 0.5f : -0.5f;
    // A zero normal: the node's own local Z, which a placement above it
    // turns with the rest
    SbVec3f n = flipNormal.getValue();
    if (n.sqrLength() == 0.f) {
        n.setValue(m[2][0], m[2][1], m[2][2]);
    }
    const float ndotz = n[0]*v[0][2] + n[1]*v[1][2] + n[2]*v[2][2];
    const bool backfacing = ndotz < 0.f;
    bool doFlip = backfacing ? (xfactor*yfactor > 0.f) : (xfactor*yfactor < 0.f);
    if (det < 0.f)
        doFlip = !doFlip;
    if (doFlip)
        xfactor = -xfactor;
    for (int k = 0; k < 3; ++k) {
        if (xfactor < 0.f)
            m[0][k] = -m[0][k];
        if (yfactor < 0.f)
            m[1][k] = -m[1][k];
    }
}

// set the auto scale factor.
//void SoAutoZoomTranslation::setAutoScale(void)
//{
//    float sf = this->getScaleFactor();
//    //this->enableNotify	(	false );
//    scaleFactor.setValue(SbVec3f(sf,sf,sf));
//    //this->enableNotify	(	true );
//    //scaleFactor.setDirty (true);
//
//}

void SoAutoZoomTranslation::getMatrix(SoGetMatrixAction * action)
{
    float sf = this->getScaleFactor(action);

    SbMatrix &m = action->getMatrix();

    SbRotation r,so;
    SbVec3f s,t;
    m.getTransform(t,r,s,so);
    m.multVecMatrix(SbVec3f(0,0,0),t);
    m.setTransform(t,r,SbVec3f(sf,sf,sf));

    action->getInverse() = m.inverse();
}

void SoAutoZoomTranslation::callback(SoCallbackAction * action)
{
    SoAutoZoomTranslation::doAction(static_cast<SoAction*>(action));
}

void SoAutoZoomTranslation::getBoundingBox(SoGetBoundingBoxAction * action)
{
    SoAutoZoomTranslation::doAction(static_cast<SoAction*>(action));
}

void SoAutoZoomTranslation::pick(SoPickAction * action)
{
    SoAutoZoomTranslation::doAction(static_cast<SoAction*>(action));
}

// Doc in superclass.
void SoAutoZoomTranslation::getPrimitiveCount(SoGetPrimitiveCountAction * action)
{
    SoAutoZoomTranslation::doAction(static_cast<SoAction*>(action));
}
