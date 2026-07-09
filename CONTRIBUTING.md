# 贡献指南

感谢您对 AI Memory Watch 项目的关注！我们欢迎各种形式的贡献。

## 如何贡献

### 报告问题

1. 使用 [Issue 模板](https://github.com/Yezi23-boop/esp/issues/new/choose) 提交问题
2. 描述清楚问题现象、复现步骤、期望行为
3. 提供相关的日志或截图

### 提交代码

1. Fork 本仓库
2. 创建功能分支：`git checkout -b feature/your-feature`
3. 提交更改：`git commit -m 'Add some feature'`
4. 推送分支：`git push origin feature/your-feature`
5. 创建 Pull Request

### 开发规范

- 遵循 ESP-IDF 编码规范
- 代码注释清晰，函数命名语义化
- 新功能需添加相应的文档说明
- 提交前确保编译通过

### 提交信息格式

```
<类型>(<范围>): <简短描述>

<详细描述>

<关联Issue>
```

类型：
- `feat`: 新功能
- `fix`: 修复问题
- `docs`: 文档更新
- `style`: 代码格式调整
- `refactor`: 代码重构
- `test`: 测试相关
- `chore`: 构建/工具相关

## 开发环境

1. 安装 [ESP-IDF v5.5+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
2. 克隆仓库：`git clone https://github.com/Yezi23-boop/esp.git`
3. 编译测试：`idf.py build`

## 联系方式

如有疑问，可通过 Issue 或 Discussion 与我们交流。
