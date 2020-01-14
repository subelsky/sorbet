
#include "common/typecase.h"
#include "core/Symbols.h"
#include "core/core.h"
#include "core/errors/resolver.h"

#include "definition_validator/variance.h"

using namespace std;

namespace sorbet::definition_validator::variance {

enum class Polarity {
    Positive = 1,
    Neutral = 0,
    Negative = -1,
};

// Show a Polarity using the same in/out notation as Variance.
string showPolarity(const Polarity polarity) {
    switch (polarity) {
        case Polarity::Positive:
            return ":out";
        case Polarity::Neutral:
            return ":invariant";
        case Polarity::Negative:
            return ":in";
    }
}

const Polarity negatePolarity(const Polarity polarity) {
    switch (polarity) {
        case Polarity::Positive:
            return Polarity::Negative;
        case Polarity::Neutral:
            return Polarity::Neutral;
        case Polarity::Negative:
            return Polarity::Positive;
    }
}

string showVariance(const core::Variance variance) {
    switch (variance) {
        case core::Variance::CoVariant:
            return ":out";
        case core::Variance::Invariant:
            return ":invariant";
        case core::Variance::ContraVariant:
            return ":in";
    }
}

bool hasCompatibleVariance(const Polarity polarity, const core::Variance argVariance) {
    switch (polarity) {
        case Polarity::Positive:
            return argVariance != core::Variance::ContraVariant;
        case Polarity::Neutral:
            return true;
        case Polarity::Negative:
            return argVariance != core::Variance::CoVariant;
    }
}

class VarianceValidator {
private:
    const core::Loc loc;

    VarianceValidator(const core::Loc loc) : loc(loc) {}

    void validate(const core::Context ctx, const Polarity polarity, const core::TypePtr type) {
        typecase(
            type.get(), [&](core::ClassType *klass) {},

            [&](core::LiteralType *lit) {},

            [&](core::SelfType *self) {},

            [&](core::SelfTypeParam *sp) {},

            [&](core::TypeVar *tvar) {},

            [&](core::ExtendsType *extn) {},

            [&](core::OrType *any) {
                validate(ctx, polarity, any->left);
                validate(ctx, polarity, any->right);
            },

            [&](core::AndType *all) {
                validate(ctx, polarity, all->left);
                validate(ctx, polarity, all->right);
            },

            [&](core::ShapeType *shape) {
                for (auto value : shape->values) {
                    validate(ctx, polarity, value);
                }
            },

            [&](core::TupleType *tuple) {
                for (auto value : tuple->elems) {
                    validate(ctx, polarity, value);
                }
            },

            [&](core::AppliedType *app) {
                auto members = app->klass.data(ctx)->typeMembers();
                auto params = app->targs;

                ENFORCE(members.size() == params.size(),
                        fmt::format("types should be fully saturated, but there are {} members and {} params",
                                    members.size(), params.size()));

                for (int i = 0; i < members.size(); ++i) {
                    auto memberVariance = members[i].data(ctx)->variance();
                    auto typeArg = params[i];

                    // The polarity used to check the parameter is negated
                    // when the parameter is defined as ContraVariant, for
                    // example:
                    //
                    // -(-a -> +b) -> +c === (+a -> -b) -> + c
                    Polarity paramPolarity;
                    switch (memberVariance) {
                        case core::Variance::ContraVariant:
                            paramPolarity = negatePolarity(polarity);
                            break;
                        case core::Variance::Invariant:
                            paramPolarity = Polarity::Neutral;
                            break;
                        case core::Variance::CoVariant:
                            paramPolarity = polarity;
                            break;
                    }

                    validate(ctx, paramPolarity, typeArg);
                }
            },

            // This is where the actual variance checks are done.
            [&](core::LambdaParam *param) {
                auto paramData = param->definition.data(ctx);

                ENFORCE(paramData->isTypeMember());

                auto paramVariance = paramData->variance();

                if (!hasCompatibleVariance(polarity, paramVariance)) {
                    if (auto e = ctx.state.beginError(this->loc, core::errors::Resolver::InvalidVariance)) {
                        auto flavor =
                            paramData->owner.data(ctx)->isSingletonClass(ctx) ? "type_template" : "type_member";

                        auto paramName = paramData->name.data(ctx)->show(ctx);

                        e.setHeader("`{}` `{}` was defined as `{}` but is used in an `{}` context", flavor, paramName,
                                    showVariance(paramVariance), showPolarity(polarity));

                        e.addErrorLine(paramData->loc(), "`{}` `{}` defined here as `{}`", flavor, paramName,
                                       showVariance(paramVariance));
                    }
                }
            },

            [&](core::AliasType *alias) {
                auto aliasSym = alias->symbol.data(ctx)->dealias(ctx);

                // This can be introduced by `module_function`, which in its
                // current implementation will alias an instance method as a
                // class method.
                if (aliasSym.data(ctx)->isMethod()) {
                    validateMethod(ctx, polarity, aliasSym);
                } else {
                    Exception::raise("Unexpected type alias: {}", alias->toString(ctx));
                }
            },

            [&](core::Type *skipped) {
                Exception::raise("Unexpected type in variance checking: {}", skipped->toString(ctx));
            });
    }

public:
    static void validatePolarity(const core::Loc loc, const core::Context ctx, const Polarity polarity,
                                 const core::TypePtr type) {
        VarianceValidator validator(loc);
        return validator.validate(ctx, polarity, type);
    }

    // Variance checking, parameterized on the external polarity of the method
    // context.
    static void validateMethod(const core::Context ctx, const Polarity polarity, const core::SymbolRef method) {
        auto methodData = method.data(ctx);

        // Negate the polarity for checking arguments in a ContraVariant
        // context.
        const Polarity negated = negatePolarity(polarity);

        for (auto &arg : methodData->arguments()) {
            if (arg.type != nullptr) {
                validatePolarity(arg.loc, ctx, negated, arg.type);
            }
        }

        if (methodData->resultType != nullptr) {
            validatePolarity(methodData->loc(), ctx, polarity, methodData->resultType);
        }
    }
};

// Validates uses of type members according to their variance.
void validateMethodVariance(const core::Context ctx, const core::SymbolRef method) {
    VarianceValidator::validateMethod(ctx, Polarity::Positive, method);
}

} // namespace sorbet::definition_validator::variance
