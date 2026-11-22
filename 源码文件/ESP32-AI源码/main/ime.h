#ifndef __IME_H
#define __IME_H

#include <Arduino.h>

/* 拼音候选查询：返回 "候选1|候选2|..."，page 从 1 开始；无结果返回空串 */
String ime_lookup(const String &py_in, int page);

#endif
