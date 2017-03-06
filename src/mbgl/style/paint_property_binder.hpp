#pragma once

#include <mbgl/programs/attributes.hpp>
#include <mbgl/gl/attribute.hpp>
#include <mbgl/gl/uniform.hpp>
#include <mbgl/util/type_list.hpp>

namespace mbgl {
namespace style {

template <class T, class A>
class ConstantPaintPropertyBinder {
public:
    using Attribute = A;

    using AttributeValue = typename Attribute::Value;
    using AttributeBinding = typename Attribute::Binding;

    ConstantPaintPropertyBinder(T constant_)
        : constant(std::move(constant_)) {
    }

    void populateVertexVector(const GeometryTileFeature&, std::size_t) {}
    void upload(gl::Context&) {}

    AttributeBinding attributeBinding(const PossiblyEvaluatedPropertyValue<T>& currentValue) const {
        return typename Attribute::ConstantBinding {
            Attribute::value(currentValue.constantOr(constant))
        };
    }

    float interpolationFactor(float) const {
        return 0.0f;
    }

private:
    T constant;
};

template <class T, class A>
class SourceFunctionPaintPropertyBinder {
public:
    using Attribute = A;
    
    using AttributeValue = typename Attribute::Value;
    using AttributeBinding = typename Attribute::Binding;

    using Attributes = gl::Attributes<Attribute>;
    using Vertex = typename Attributes::Vertex;

    SourceFunctionPaintPropertyBinder(SourceFunction<T> function_, T defaultValue_)
        : function(std::move(function_)),
          defaultValue(std::move(defaultValue_)) {
    }

    void populateVertexVector(const GeometryTileFeature& feature, std::size_t length) {
        auto val = function.evaluate(feature, defaultValue);
        AttributeValue value = Attribute::value(val);
        for (std::size_t i = vertexVector.vertexSize(); i < length; ++i) {
            vertexVector.emplace_back(Vertex { value });
        }
    }

    void upload(gl::Context& context) {
        vertexBuffer = context.createVertexBuffer(std::move(vertexVector));
    }

    AttributeBinding attributeBinding(const PossiblyEvaluatedPropertyValue<T>& currentValue) const {
        if (currentValue.isConstant()) {
            return typename Attribute::ConstantBinding {
                Attribute::value(*currentValue.constant())
            };
        } else {
            return Attributes::allVariableBindings(*vertexBuffer)
                .template get<Attribute>();
        }
    }

    float interpolationFactor(float) const {
        return 0.0f;
    }

private:
    SourceFunction<T> function;
    T defaultValue;
    gl::VertexVector<Vertex> vertexVector;
    optional<gl::VertexBuffer<Vertex>> vertexBuffer;
};

template <class T, class A>
class CompositeFunctionPaintPropertyBinder {
public:
    using Attribute = attributes::ZoomInterpolatedAttribute<A>;
    
    using AttributeValue = typename Attribute::Value;
    using AttributeBinding = typename Attribute::Binding;

    using Attributes = gl::Attributes<Attribute>;
    using Vertex = typename Attributes::Vertex;

    CompositeFunctionPaintPropertyBinder(CompositeFunction<T> function_, float zoom, T defaultValue_)
        : function(std::move(function_)),
          defaultValue(std::move(defaultValue_)),
          coveringRanges(function.coveringRanges(zoom)) {
    }

    void populateVertexVector(const GeometryTileFeature& feature, std::size_t length) {
        Range<T> range = function.evaluate(std::get<1>(coveringRanges), feature, defaultValue);
        AttributeValue minMax = Attribute::value(range.min, range.max);
        for (std::size_t i = vertexVector.vertexSize(); i < length; ++i) {
            vertexVector.emplace_back(Vertex { minMax });
        }
    }

    void upload(gl::Context& context) {
        vertexBuffer = context.createVertexBuffer(std::move(vertexVector));
    }

    AttributeBinding attributeBinding(const PossiblyEvaluatedPropertyValue<T>& currentValue) const {
        if (currentValue.isConstant()) {
            auto val = *currentValue.constant();
            return typename Attribute::ConstantBinding {
                Attribute::value(val, val)
            };
        } else {
            return Attributes::allVariableBindings(*vertexBuffer)
                .template get<Attribute>();
        }
    }

    float interpolationFactor(float currentZoom) const {
        return util::interpolationFactor(1.0f, std::get<0>(coveringRanges), currentZoom);
    }

private:
    using InnerStops = typename CompositeFunction<T>::InnerStops;
    CompositeFunction<T> function;
    T defaultValue;
    std::tuple<Range<float>, Range<InnerStops>> coveringRanges;
    gl::VertexVector<Vertex> vertexVector;
    optional<gl::VertexBuffer<Vertex>> vertexBuffer;
};

template <class PaintProperty>
class PaintPropertyBinder {
public:
    using Type = typename PaintProperty::Type;
    using Attribute = typename PaintProperty::Attribute;
    using PropertyValue = typename PaintProperty::EvaluatedType;

    using Binder = variant<
        ConstantPaintPropertyBinder<Type, Attribute>,
        SourceFunctionPaintPropertyBinder<Type, Attribute>,
        CompositeFunctionPaintPropertyBinder<Type, Attribute>>;

    using AttributeBinding = variant<
        typename Attribute::Binding,
        typename attributes::ZoomInterpolatedAttribute<Attribute>::Binding>;
    
    PaintPropertyBinder(const PropertyValue& value, float zoom)
        : binder(value.match(
            [&] (const Type& constant) -> Binder {
                return ConstantPaintPropertyBinder<Type, Attribute>(constant);
            },
            [&] (const SourceFunction<Type>& function) {
                return SourceFunctionPaintPropertyBinder<Type, Attribute>(function, PaintProperty::defaultValue());
            },
            [&] (const CompositeFunction<Type>& function) {
                return CompositeFunctionPaintPropertyBinder<Type, Attribute>(function, zoom, PaintProperty::defaultValue());
            }
        )) {
    }

    void populateVertexVector(const GeometryTileFeature& feature, std::size_t length) {
        binder.match([&] (auto& b) {
            b.populateVertexVector(feature, length);
        });
    }

    void upload(gl::Context& context) {
        binder.match([&] (auto& b) {
            b.upload(context);
        });
    }

    AttributeBinding attributeBinding(const PropertyValue& currentValue) const {
        return binder.match([&] (const auto& b) {
            return b.attributeBinding(currentValue);
        });
    }

    using InterpolationUniform = attributes::InterpolationUniform<Attribute>;
    using InterpolationUniformValue = typename InterpolationUniform::Value;

    InterpolationUniformValue interpolationUniformValue(float currentZoom) const {
        return InterpolationUniformValue {
            binder.match([&] (const auto& b) {
                return b.interpolationFactor(currentZoom);
            })
        };
    }

private:
    Binder binder;
};

template <class Ps>
class PaintPropertyBinders;

template <class... Ps>
class PaintPropertyBinders<TypeList<Ps...>> {
public:
    using Binders = IndexedTuple<TypeList<Ps...>, TypeList<PaintPropertyBinder<Ps>...>>;

    template <class EvaluatedProperties>
    PaintPropertyBinders(const EvaluatedProperties& properties, float z)
        : binders(PaintPropertyBinder<Ps>(properties.template get<Ps>(), z)...) {
        (void)z; // Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56958
    }

    void populateVertexVectors(const GeometryTileFeature& feature, std::size_t length) {
        util::ignore({
            (binders.template get<Ps>().populateVertexVector(feature, length), 0)...
        });
    }

    void upload(gl::Context& context) {
        util::ignore({
            (binders.template get<Ps>().upload(context), 0)...
        });
    }

    using Attributes = gl::Attributes<typename PaintPropertyBinder<Ps>::Attribute...>;

    using AttributeBindings = typename Attributes::Bindings;

    template <class EvaluatedProperties>
    AttributeBindings attributeBindings(const EvaluatedProperties& currentProperties) const {
        return typename Attributes::Bindings {
            binders.template get<Ps>().attributeBinding(currentProperties.template get<Ps>())...
        };
    }

    using Uniforms = gl::Uniforms<typename PaintPropertyBinder<Ps>::InterpolationUniform...>;
    using UniformValues = typename Uniforms::Values;

    UniformValues uniformValues(float currentZoom) const {
        (void)currentZoom; // Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56958
        return UniformValues {
            binders.template get<Ps>().interpolationUniformValue(currentZoom)...
        };
    }

private:
    Binders binders;
};

} // namespace style
} // namespace mbgl
