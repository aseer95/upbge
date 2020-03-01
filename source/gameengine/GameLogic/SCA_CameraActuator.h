/*
 * SCA_CameraActuator.h
 *
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file SCA_CameraActuator.h
 *  \ingroup ketsji
 */

#ifndef __SCA_CameraActuator_H__
#define __SCA_CameraActuator_H__

#include "SCA_IActuator.h"
#include "MT_Scalar.h"
#include "SCA_LogicManager.h"

/**
 * The camera actuator does a Robbie Muller perspective for you. This is a
 * weird set of rules that positions the camera sort of behind the object,
 * tracking, while avoiding any objects between the 'ideal' position and the
 * actor being tracked.
 */

class SCA_CameraActuator : public SCA_IActuator {
  Py_Header private :
      /** Object that will be tracked. */
      SCA_IObject *m_ob;

  /** height (float), */
  // const MT_Scalar m_height;
  /** min (float), */
  // const MT_Scalar m_minHeight;
  /** max (float), */
  // const MT_Scalar m_maxHeight;

  /** height (float), */
  float m_height;

  /** min (float), */
  float m_minHeight;

  /** max (float), */
  float m_maxHeight;

  /** axis the camera tries to get behind: +x/+y/-x/-y */
  short m_axis;

  /** damping (float), */
  float m_damping;

 public:
  static std::string X_AXIS_STRING;
  static std::string Y_AXIS_STRING;

  /**
   * Set the bool toggle to true to use x lock, false for y lock
   */
  SCA_CameraActuator(

      SCA_IObject *gameobj,
      // const CValue *ob,
      SCA_IObject *ob,
      float hght,
      float minhght,
      float maxhght,
      short axis,
      float damping);

  ~SCA_CameraActuator();

  /** Methods Inherited from  CValue */
  CValue *GetReplica();
  virtual void ProcessReplica();

  /** Methods inherited from SCA_IActuator */
  virtual bool Update(double curtime);
  virtual bool UnlinkObject(SCA_IObject *clientobj);

  /** Methods inherited from SCA_ILogicBrick */
  virtual void Relink(std::map<SCA_IObject *, SCA_IObject *> &obj_map);

#ifdef WITH_PYTHON

  /* --------------------------------------------------------------------- */
  /* Python interface ---------------------------------------------------- */
  /* --------------------------------------------------------------------- */

  /* set object to look at */
  static PyObject *pyattr_get_object(PyObjectPlus *self_v, const KX_PYATTRIBUTE_DEF *attrdef);
  static int pyattr_set_object(PyObjectPlus *self_v,
                               const KX_PYATTRIBUTE_DEF *attrdef,
                               PyObject *value);

#endif /* WITH_PYTHON */
};

#endif /* __SCA_CameraActuator_H__ */
