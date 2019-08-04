
#ifndef PXSCENE_MSEUTILS_H
#define PXSCENE_MSEUTILS_H

#include "rtObject.h"
#include "MSEBaseObject.h"
#include <vector>

/**
 * vector array to rt arr
 * @param items the vector items
 * @return the rt array
 */
rtArrayObject *vectorToRTArr(const std::vector<MSEBaseObject *> &items);


/**
 * fill vector from rtArr
 * @param rtArr  the rtArr
 * @param items the vector
 */
void fillVectorFromRTArr(rtObjectRef const &rtArr, std::vector<MSEBaseObject *> &items);

#define defineReadOnlyAttribute(field, classField, fType) \
  rtReadOnlyProperty(field, get_ ## field, fType); \
  rtError get_ ## field(fType &val) const { val = classField; return RT_OK; }

#define defineReadWriteAttribute(field, classField, fType) \
  rtProperty(field, get_ ## field, set_ ## field, fType); \
  rtError get_ ## field(fType &val) const { val = classField; return RT_OK; } \
  rtError set_ ## field(const fType &val) { classField = val; return RT_OK; } \

#define defineEventDefinition(eventName) \
  void on_ ## eventName() { mEmit.send(#eventName); }


#endif
