/**************************************************************************/
/*  rt_alpha_dependencies.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "rt_alpha_dependencies.h"

namespace {

using SL = ShaderLanguage;

// Symbols:
//   "<function>::<name>"  a local variable or argument of a function; "<function>::@return", "@discard", "@control"
//                         (the conditions of its early returns, breaks and continues), "@context" (of its call sites).
//   "IN:<name>"           a vertex attribute as the mesh stores it (vertex() reads it, or reads what it wrote).
//   "V:<name>"            what vertex() writes to a built-in.
//   "F:<name>"            a fragment() built-in: its outputs, and its inputs (UV, UV2 and COLOR come from vertex()).
//   "B:<name>"            a built-in read outside the stage functions.
//   "VAR:<name>"          a varying.
//   "U:<name>"            a uniform.
//   "C:<name>"            a constant.
class Analyzer {
	const SL::ShaderNode *shader = nullptr;
	HashMap<String, HashSet<String>> deps;
	String function; // The function being visited.
	enum Stage {
		STAGE_OTHER,
		STAGE_VERTEX,
		STAGE_FRAGMENT,
	};
	Stage stage = STAGE_OTHER;
	LocalVector<HashSet<String>> conditions; // Of the enclosing control flows.
	HashSet<String> function_targets; // Assigned in the function being visited.

	void _add(const String &p_target, const HashSet<String> &p_sources) {
		HashSet<String> &d = deps[p_target];
		for (const String &s : p_sources) {
			d.insert(s);
		}
	}

	void _add(const String &p_target, const String &p_source) {
		deps[p_target].insert(p_source);
	}

	HashSet<String> _current_conditions() const {
		HashSet<String> result;
		for (const HashSet<String> &c : conditions) {
			for (const String &s : c) {
				result.insert(s);
			}
		}
		// Whether the function runs at all.
		result.insert(function + "::@context");
		return result;
	}

	String _symbol(const StringName &p_name, bool p_is_local) {
		if (p_is_local) {
			return function + "::" + String(p_name);
		}
		if (shader->uniforms.has(p_name)) {
			return "U:" + String(p_name);
		}
		if (shader->varyings.has(p_name)) {
			return "VAR:" + String(p_name);
		}
		if (shader->constants.has(p_name)) {
			return "C:" + String(p_name);
		}
		switch (stage) {
			case STAGE_VERTEX: {
				// What vertex() reads of a built-in: the mesh's value, or what it wrote before.
				const String written = "V:" + String(p_name);
				deps[written].insert("IN:" + String(p_name));
				return written;
			}
			case STAGE_FRAGMENT: {
				const String name = "F:" + String(p_name);
				if (p_name == SNAME("UV") || p_name == SNAME("UV2") || p_name == SNAME("COLOR")) {
					deps[name].insert("V:" + String(p_name));
					deps["V:" + String(p_name)].insert("IN:" + String(p_name));
				}
				return name;
			}
			default:
				return "B:" + String(p_name);
		}
	}

	// The symbol an assignment writes, and what selects the written part (indices).
	bool _target(const SL::Node *p_node, String &r_target, HashSet<String> &r_selectors) {
		switch (p_node->type) {
			case SL::Node::NODE_TYPE_VARIABLE: {
				const SL::VariableNode *v = static_cast<const SL::VariableNode *>(p_node);
				r_target = _symbol(v->name, v->is_local);
				return true;
			}
			case SL::Node::NODE_TYPE_ARRAY: {
				const SL::ArrayNode *a = static_cast<const SL::ArrayNode *>(p_node);
				r_target = _symbol(a->name, a->is_local);
				if (a->index_expression) {
					_merge(r_selectors, _reads(a->index_expression));
				}
				return true;
			}
			case SL::Node::NODE_TYPE_MEMBER: {
				const SL::MemberNode *m = static_cast<const SL::MemberNode *>(p_node);
				if (m->index_expression) {
					_merge(r_selectors, _reads(m->index_expression));
				}
				return _target(m->owner, r_target, r_selectors);
			}
			case SL::Node::NODE_TYPE_OPERATOR: {
				const SL::OperatorNode *op = static_cast<const SL::OperatorNode *>(p_node);
				if (op->op == SL::OP_INDEX && op->arguments.size() == 2) {
					_merge(r_selectors, _reads(op->arguments[1]));
					return _target(op->arguments[0], r_target, r_selectors);
				}
			} break;
			default:
				break;
		}
		return false;
	}

	static void _merge(HashSet<String> &r_into, const HashSet<String> &p_from) {
		for (const String &s : p_from) {
			r_into.insert(s);
		}
	}

	void _assign(const String &p_target, const HashSet<String> &p_sources) {
		_add(p_target, p_sources);
		_add(p_target, _current_conditions());
		function_targets.insert(p_target);
	}

public:
	bool failed = false;
	String failure;

	// Everything an expression reads; records the assignments it makes.
	HashSet<String> _reads(const SL::Node *p_node) {
		HashSet<String> result;
		if (!p_node) {
			return result;
		}
		switch (p_node->type) {
			case SL::Node::NODE_TYPE_CONSTANT: {
				const SL::ConstantNode *c = static_cast<const SL::ConstantNode *>(p_node);
				for (const SL::VariableDeclarationNode::Declaration &d : c->array_declarations) {
					for (const SL::Node *init : d.initializer) {
						_merge(result, _reads(init));
					}
				}
			} break;
			case SL::Node::NODE_TYPE_VARIABLE: {
				const SL::VariableNode *v = static_cast<const SL::VariableNode *>(p_node);
				result.insert(_symbol(v->name, v->is_local));
			} break;
			case SL::Node::NODE_TYPE_ARRAY: {
				const SL::ArrayNode *a = static_cast<const SL::ArrayNode *>(p_node);
				const String symbol = _symbol(a->name, a->is_local);
				result.insert(symbol);
				_merge(result, _reads(a->index_expression));
				_merge(result, _reads(a->call_expression));
				if (a->assign_expression) {
					HashSet<String> sources = _reads(a->assign_expression);
					_merge(sources, _reads(a->index_expression));
					_assign(symbol, sources);
				}
			} break;
			case SL::Node::NODE_TYPE_ARRAY_CONSTRUCT: {
				const SL::ArrayConstructNode *ac = static_cast<const SL::ArrayConstructNode *>(p_node);
				for (const SL::Node *init : ac->initializer) {
					_merge(result, _reads(init));
				}
			} break;
			case SL::Node::NODE_TYPE_MEMBER: {
				const SL::MemberNode *m = static_cast<const SL::MemberNode *>(p_node);
				_merge(result, _reads(m->owner));
				_merge(result, _reads(m->index_expression));
				_merge(result, _reads(m->call_expression));
				if (m->assign_expression) {
					String target;
					HashSet<String> selectors;
					if (_target(m->owner, target, selectors)) {
						_merge(selectors, _reads(m->assign_expression));
						_assign(target, selectors);
					} else {
						failed = true;
						failure = "an assignment to a member it cannot follow";
					}
				}
			} break;
			case SL::Node::NODE_TYPE_OPERATOR: {
				const SL::OperatorNode *op = static_cast<const SL::OperatorNode *>(p_node);
				switch (op->op) {
					case SL::OP_ASSIGN:
					case SL::OP_ASSIGN_ADD:
					case SL::OP_ASSIGN_SUB:
					case SL::OP_ASSIGN_MUL:
					case SL::OP_ASSIGN_DIV:
					case SL::OP_ASSIGN_MOD:
					case SL::OP_ASSIGN_SHIFT_LEFT:
					case SL::OP_ASSIGN_SHIFT_RIGHT:
					case SL::OP_ASSIGN_BIT_AND:
					case SL::OP_ASSIGN_BIT_OR:
					case SL::OP_ASSIGN_BIT_XOR:
					case SL::OP_INCREMENT:
					case SL::OP_DECREMENT:
					case SL::OP_POST_INCREMENT:
					case SL::OP_POST_DECREMENT: {
						String target;
						HashSet<String> sources;
						if (op->arguments.is_empty() || !_target(op->arguments[0], target, sources)) {
							failed = true;
							failure = "an assignment it cannot follow";
							break;
						}
						for (int i = 1; i < op->arguments.size(); i++) {
							_merge(sources, _reads(op->arguments[i]));
						}
						if (op->op != SL::OP_ASSIGN) {
							sources.insert(target); // A compound assignment reads its target.
						}
						_assign(target, sources);
						_merge(result, sources);
						result.insert(target);
					} break;
					case SL::OP_CALL: {
						if (op->arguments.is_empty() || op->arguments[0]->type != SL::Node::NODE_TYPE_VARIABLE) {
							failed = true;
							failure = "a call it cannot follow";
							break;
						}
						const StringName name = static_cast<const SL::VariableNode *>(op->arguments[0])->name;
						const SL::ShaderNode::Function *callee = shader->functions.getptr(name);
						if (!callee || !callee->function) {
							// A built-in function: its value depends on its arguments.
							for (int i = 1; i < op->arguments.size(); i++) {
								_merge(result, _reads(op->arguments[i]));
							}
							break;
						}
						const String callee_name = String(name);
						_add(callee_name + "::@context", _current_conditions());
						const SL::FunctionNode *fn = callee->function;
						for (int i = 1; i < op->arguments.size(); i++) {
							const HashSet<String> argument = _reads(op->arguments[i]);
							_merge(result, argument);
							if (i - 1 >= fn->arguments.size()) {
								continue;
							}
							const SL::FunctionNode::Argument &parameter = fn->arguments[i - 1];
							const String parameter_symbol = callee_name + "::" + String(parameter.name);
							if (parameter.qualifier != SL::ARGUMENT_QUALIFIER_OUT) {
								_add(parameter_symbol, argument);
							}
							if (parameter.qualifier != SL::ARGUMENT_QUALIFIER_IN) {
								String target;
								HashSet<String> selectors;
								if (!_target(op->arguments[i], target, selectors)) {
									failed = true;
									failure = "an out argument it cannot follow";
									continue;
								}
								selectors.insert(parameter_symbol);
								_assign(target, selectors);
							}
						}
						result.insert(callee_name + "::@return");
					} break;
					case SL::OP_CONSTRUCT:
					case SL::OP_STRUCT: {
						// The first argument names the type.
						for (int i = 1; i < op->arguments.size(); i++) {
							_merge(result, _reads(op->arguments[i]));
						}
					} break;
					default: {
						for (const SL::Node *argument : op->arguments) {
							_merge(result, _reads(argument));
						}
					} break;
				}
			} break;
			default: {
				failed = true;
				failure = "an expression it cannot follow";
			} break;
		}
		return result;
	}

	void _statement(const SL::Node *p_node) {
		if (!p_node || failed) {
			return;
		}
		switch (p_node->type) {
			case SL::Node::NODE_TYPE_BLOCK: {
				const SL::BlockNode *block = static_cast<const SL::BlockNode *>(p_node);
				for (const SL::Node *statement : block->statements) {
					_statement(statement);
				}
			} break;
			case SL::Node::NODE_TYPE_VARIABLE_DECLARATION: {
				const SL::VariableDeclarationNode *declaration = static_cast<const SL::VariableDeclarationNode *>(p_node);
				for (const SL::VariableDeclarationNode::Declaration &d : declaration->declarations) {
					HashSet<String> sources = _reads(d.size_expression);
					for (const SL::Node *init : d.initializer) {
						_merge(sources, _reads(init));
					}
					_assign(function + "::" + String(d.name), sources);
				}
			} break;
			case SL::Node::NODE_TYPE_CONTROL_FLOW: {
				const SL::ControlFlowNode *flow = static_cast<const SL::ControlFlowNode *>(p_node);
				HashSet<String> condition;
				for (const SL::Node *expression : flow->expressions) {
					_merge(condition, _reads(expression));
				}
				switch (flow->flow_op) {
					case SL::FLOW_OP_RETURN: {
						HashSet<String> returned(condition);
						_merge(returned, _current_conditions());
						_add(function + "::@return", returned);
						// What comes after an early return depends on whether it was taken.
						_add(function + "::@control", _current_conditions());
					} break;
					case SL::FLOW_OP_BREAK:
					case SL::FLOW_OP_CONTINUE: {
						_add(function + "::@control", _current_conditions());
					} break;
					case SL::FLOW_OP_DISCARD: {
						_add(function + "::@discard", _current_conditions());
					} break;
					case SL::FLOW_OP_FOR: {
						// The condition block is read by every iteration: it conditions the whole loop.
						HashSet<String> loop_condition(condition);
						if (flow->blocks.size() > 1 && flow->blocks[1]) {
							for (const SL::Node *statement : flow->blocks[1]->statements) {
								_merge(loop_condition, _reads(statement));
							}
						}
						conditions.push_back(HashSet<String>(loop_condition));
						for (const SL::BlockNode *block : flow->blocks) {
							_statement(block);
						}
						conditions.resize(conditions.size() - 1);
					} break;
					default: {
						// if, while, do, switch, case, default: their expressions condition their blocks.
						conditions.push_back(HashSet<String>(condition));
						for (const SL::BlockNode *block : flow->blocks) {
							_statement(block);
						}
						conditions.resize(conditions.size() - 1);
					} break;
				}
			} break;
			default: {
				_reads(p_node);
			} break;
		}
	}

	void visit_function(const SL::FunctionNode *p_function) {
		function = String(p_function->name);
		stage = p_function->name == SNAME("vertex") ? STAGE_VERTEX : (p_function->name == SNAME("fragment") ? STAGE_FRAGMENT : STAGE_OTHER);
		function_targets.clear();
		conditions.clear();
		_statement(p_function->body);
		// Early returns, breaks and continues: every value the function assigns may depend on them.
		for (const String &target : function_targets) {
			deps[target].insert(function + "::@control");
		}
	}

	explicit Analyzer(const SL::ShaderNode *p_shader) :
			shader(p_shader) {}

	// The inputs the roots depend on, transitively.
	HashSet<String> leaves(const Vector<String> &p_roots) const {
		HashSet<String> visited;
		HashSet<String> result;
		LocalVector<String> stack;
		for (const String &root : p_roots) {
			stack.push_back(root);
		}
		while (!stack.is_empty()) {
			const String symbol = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			if (visited.has(symbol)) {
				continue;
			}
			visited.insert(symbol);
			const HashSet<String> *sources = deps.getptr(symbol);
			if (!sources) {
				result.insert(symbol);
				continue;
			}
			for (const String &source : *sources) {
				stack.push_back(source);
			}
		}
		return result;
	}
};

} //namespace

RTAlphaDependencies rt_alpha_dependencies(const ShaderLanguage::ShaderNode *p_shader) {
	RTAlphaDependencies result;
	if (!p_shader) {
		result.reason = "no shader";
		return result;
	}

	Analyzer analyzer(p_shader);
	for (const ShaderLanguage::ShaderNode::Function &f : p_shader->vfunctions) {
		if (f.function && f.name != SNAME("light")) {
			analyzer.visit_function(f.function);
		}
	}
	if (analyzer.failed) {
		result.reason = analyzer.failure;
		return result;
	}

	// What the any-hit shaders test: ALPHA against ALPHA_SCISSOR_THRESHOLD, and the discards of fragment() and of the
	// functions it calls.
	Vector<String> roots = { "F:ALPHA", "F:ALPHA_SCISSOR_THRESHOLD", "fragment::@discard" };
	for (const ShaderLanguage::ShaderNode::Function &f : p_shader->vfunctions) {
		if (f.function && f.function->can_discard) {
			roots.push_back(String(f.name) + "::@discard");
		}
	}

	const bool world_vertex_coords = p_shader->render_modes.has(SNAME("world_vertex_coords"));
	for (const String &leaf : analyzer.leaves(roots)) {
		bool fixed = false;
		const int colon = leaf.find(":");
		const String unprefixed = colon >= 0 ? leaf.substr(colon + 1) : leaf;
		if (unprefixed == "PI" || unprefixed == "TAU" || unprefixed == "E" || unprefixed == "OUTPUT_IS_SRGB") {
			fixed = true; // Constants, in any stage.
		} else if (leaf.contains("::")) {
			fixed = true; // A local never assigned, a parameter never passed, a function never called.
		} else if (leaf == "F:ALPHA" || leaf == "F:ALPHA_SCISSOR_THRESHOLD") {
			fixed = true; // Never written: its default.
		} else if (leaf.begins_with("U:")) {
			const ShaderLanguage::ShaderNode::Uniform *uniform = p_shader->uniforms.getptr(StringName(leaf.substr(2)));
			fixed = uniform && uniform->scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_LOCAL &&
					uniform->hint != ShaderLanguage::ShaderNode::Uniform::HINT_SCREEN_TEXTURE &&
					uniform->hint != ShaderLanguage::ShaderNode::Uniform::HINT_DEPTH_TEXTURE &&
					uniform->hint != ShaderLanguage::ShaderNode::Uniform::HINT_NORMAL_ROUGHNESS_TEXTURE &&
					uniform->hint < ShaderLanguage::ShaderNode::Uniform::HINT_BLIT_SOURCE0;
			if (fixed) {
				result.uniforms.push_back(StringName(leaf.substr(2)));
			}
		} else if (leaf.begins_with("C:")) {
			fixed = true;
		} else if (leaf.begins_with("IN:")) {
			const String name = leaf.substr(3);
			// The mesh's attributes, in its own space (world_vertex_coords gives VERTEX and the vectors in world space).
			fixed = name == "UV" || name == "UV2" || name == "COLOR" || name == "VERTEX_ID" || name.begins_with("CUSTOM") ||
					(!world_vertex_coords && (name == "VERTEX" || name == "NORMAL" || name == "TANGENT" || name == "BINORMAL"));
			if (name == "COLOR") {
				result.reads_color = true;
			}
		}
		if (!fixed) {
			result.reason = leaf;
			return result;
		}
	}
	result.bakeable = true;
	return result;
}
