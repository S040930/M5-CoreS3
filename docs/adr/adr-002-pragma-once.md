# ADR-002: 统一 #pragma once 头文件保护

## 状态
Accepted (2026-05-01)

## 背景
重构前代码库中头文件保护方式不一致，部分使用 `#ifndef` / `#define` / `#endif`，部分使用 `#pragma once`。

## 决策
所有头文件统一使用 `#pragma once`。

### 理由
- 更简洁，减少样板代码
- 避免头文件保护宏命名冲突
- 所有主流编译器均支持（GCC、Clang、MSVC）
- ESP-IDF 项目标准实践

## 影响
- 已存在的 `#ifndef` 守卫需全部转换（重构后已全部完成）
- 新头文件创建时必须使用 `#pragma once`
