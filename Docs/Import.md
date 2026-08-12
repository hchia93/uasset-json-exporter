# Import

读 JSON spec，把控件树写回 Widget Blueprint。

RunName: `WidgetLayoutImport`
参数: `-spec="<spec.json 绝对路径>"`

## spec 格式

顶层两个字段。

| 字段 | 含义 |
| --- | --- |
| `AssetPath` | 目标 Widget Blueprint 的资产路径 |
| `WidgetTree` | 根节点，往下递归 |

节点字段。

| 字段 | 含义 |
| --- | --- |
| `Class` | 裸 UMG 类名如 `HorizontalBox`，或 Blueprint 类的完整对象路径如 `/Game/UI/WBP_Bar.WBP_Bar_C` |
| `Name` | 控件名，C++ 的 `BindWidget` 靠它匹配 |
| `Properties` | 属性名到字符串值，走反射 `ImportText` |
| `Slot` | 该节点在父容器里的 slot 属性，同样走 `ImportText` |
| `Children` | 子节点数组，父节点必须是 panel 类型 |

属性值字符串就是 Export 导出的那种形式，例 `(Value=1.000000,SizeRule=Fill)`、`(Right=48.000000)`、`HAlign_Fill`。

最小示例。

```json
{
  "AssetPath": "/Game/UI/WBP_Foo",
  "WidgetTree": {
    "Class": "HorizontalBox",
    "Name": "RootBox",
    "Children": [
      {
        "Class": "SizeBox",
        "Name": "IconBox",
        "Properties": {
          "bOverride_WidthOverride": "True",
          "WidthOverride": "64.000000"
        },
        "Slot": {
          "Padding": "(Right=48.000000)",
          "VerticalAlignment": "VAlign_Center"
        },
        "Children": [
          {
            "Class": "Image",
            "Name": "Icon",
            "Properties": {
              "ColorAndOpacity": "(R=1.000000,G=1.000000,B=1.000000,A=1.000000)"
            }
          }
        ]
      },
      {
        "Class": "TextBlock",
        "Name": "Label",
        "Properties": {
          "Text": "INVTEXT(\"Label\")"
        },
        "Slot": {
          "Size": "(Value=1.000000,SizeRule=Fill)",
          "HorizontalAlignment": "HAlign_Fill"
        }
      }
    ]
  }
}
```

## 调用

目标资产路径放 `AssetList`，用于通知显示，spec 走 `EXTRA_ARGS`。

```bash
bash Plugins/UAssetWorkbench/scripts/run_commandlet.sh \
    "<UE_PATH>" \
    "<PROJECT_DIR>/MyProject.uproject" \
    WidgetLayoutImport \
    "/Game/UI/WBP_Foo" \
    10 600 \
    '-spec="C:/temp/WBP_Foo.spec.json"'
```

## round-trip

`WidgetLayoutExport` 的产物可以直接当 Import 的输入，Import 只读它认识的字段，其余原样忽略。

改布局的常规做法: 先 Export 拿到当前树，改 JSON，再 Import 写回。

## 行为约定

| 约定 | 说明 |
| --- | --- |
| 整树替换 | 不是增量合并，spec 必须描述完整的树，旧根节点在保存时脱落 |
| 跳过 export-only 字段 | `Slots` 是 `AddChild` 重建的活对象列表，`SlotClass` 是导出侧的元数据，喂回去会破坏树，Import 直接跳过 |
| 编译检查 | 导入后编译 Blueprint 并检查状态，`BindWidget` 的名字或类型对不上会在这里报错 |
| 属性失败不致命 | 单个属性 `ImportText` 失败只给 warning，其余照常写入，日志在 `LogUAssetWorkbenchImporter` |

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 写回并编译通过 |
| 1 | spec 解析失败、编译报错，或参数错误 |
| 2 | 编辑器在运行 |

Import 组不会返回 3，3 是 Audit 组专用。
