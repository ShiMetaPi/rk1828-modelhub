# -*- coding: utf-8 -*-
"""PC 侧纯逻辑测试：正则抽取 + system 组装。板上不跑本文件。
Run: python test_memory_logic.py（预期全 PASS）
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from memory import extract_facts, compose_system

def test_extract_name():
    facts = extract_facts("你好，我叫和松溪，请问你是谁？")
    assert any("和松溪" in f for f in facts), facts

def test_extract_name_alt():
    facts = extract_facts("我的名字是hsx")
    assert any("hsx" in f for f in facts), facts

def test_extract_none():
    assert extract_facts("99乘4等于多少？") == []

def test_compose_both():
    s = compose_system(["用户名字：和松溪"], "［此前对话摘要］问:x 答:y。")
    assert s.startswith("已知用户信息") and "摘要" in s

def test_compose_memory_only():
    s = compose_system(["用户名字：和松溪"], "")
    assert "和松溪" in s and "摘要" not in s

def test_compose_none():
    assert compose_system([], "") == ""

if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn(); print("PASS", fn.__name__)
    print("ALL-%d-PASS" % len(fns))
