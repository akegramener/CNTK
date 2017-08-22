#pragma once

#pragma warning(push)
#pragma warning(disable : 4100 4244 4458 4127)
#include "Halide.h"
#pragma warning(pop)

namespace CNTK
{
    const int c_VectorizationWidth = 4;

    inline Halide::Func VectorByMatrixTimes(Halide::Func vec, Halide::Func matrix, int matrixRowDimension, int matrixColumnDimension)
    {
        if (matrixRowDimension < c_VectorizationWidth)
        {
            // No point in vectorization, the size is too small.
            Halide::Func output("VectorByMatrixTimes");
            Halide::RDom k(0, matrixRowDimension, "matrixRowIndex");
            Halide::Var matrixColumnIndex("matrixColumnIndex");
            output(matrixColumnIndex) = Halide::sum(vec(k) * matrix(matrixColumnIndex, k));
            return output;
        }

        Halide::Func partial("VectorByMatrixTimesPartial");
        Halide::Var matrixSubRowIndex("matrixSubRowIndex");
        Halide::Var matrixColumnIndex("matrixColumnIndex");
        Halide::RDom k1(0, matrixRowDimension / c_VectorizationWidth);
        Halide::Expr partialMul = vec(matrixSubRowIndex + (k1 * c_VectorizationWidth)) * matrix(matrixColumnIndex, matrixSubRowIndex + (k1 * c_VectorizationWidth));
        partial(matrixSubRowIndex, matrixColumnIndex) = Halide::sum(partialMul);
        partial.bound(matrixSubRowIndex, 0, c_VectorizationWidth);

        Halide::Func head("VectorByMatrixTimesHead");
        Halide::RDom k2(0, c_VectorizationWidth);
        head(matrixColumnIndex) = Halide::sum(partial(k2, matrixColumnIndex));

        Halide::Func tail("VectorByMatrixTimesTail");
        Halide::RDom k3((matrixRowDimension / c_VectorizationWidth) * c_VectorizationWidth, matrixRowDimension  % c_VectorizationWidth);
        auto tailMul = vec(k3) * matrix(matrixColumnIndex, k3);
        tail(matrixColumnIndex) = Halide::sum(tailMul);

        Halide::Func output("MatrixByVectorTimes");
        output(matrixColumnIndex) = head(matrixColumnIndex) + tail(matrixColumnIndex);

        partial.compute_at(output, matrixColumnIndex).vectorize(matrixSubRowIndex);

        output.bound(matrixColumnIndex, 0, matrixColumnDimension);
        output.compute_root().vectorize(matrixColumnIndex, c_VectorizationWidth);
        return output;
    }

    template <typename T>
    inline Halide::Func Sigmoid(const Halide::Func& input)
    {
        Halide::Func sigmoidOutput("Sigmoid");
        Halide::Var index;
        sigmoidOutput(index) = (T)1 / ((T)1 + fast_exp(-input(index)));
        return sigmoidOutput;
    }

    inline Halide::Func Tanh(const Halide::Func& input)
    {
        Halide::Func tanhOutput("Tanh");
        Halide::Var index;
        tanhOutput(index) = tanh(input(index));
        return tanhOutput;
    }

    inline Halide::Func Log(const Halide::Func& input)
    {
        Halide::Func logOutput("Log");
        Halide::Var index;
        logOutput(index) = log(input(index));
        return logOutput;
    }

    inline Halide::Func Slice(const Halide::Func& input, int from, int to)
    {
        Halide::Func slice("Slice");
        Halide::Var index;
        slice(index) = input(Halide::min(from + index, to - 1));
        return slice;
    }

    inline Halide::Func ElementTimes(const Halide::Func& operand1, const Halide::Func& operand2)
    {
        Halide::Var index;
        Halide::Func result("ElementTimes");
        result(index) = operand1(index) * operand2(index);
        return result;
    }

    inline Halide::Func Plus(const Halide::Func& operand1, const Halide::Func& operand2)
    {
        Halide::Var index;
        Halide::Func result("Plus");
        result(index) = operand1(index) + operand2(index);
        return result;
    }

    inline Halide::Func VectorByMatrixTimesQuantized(
        const std::vector<Halide::Func>& vec,
        const std::vector<Halide::Func>& matrix,
        int matrixRowDimension,
        int matrixColumnDimension)
    {
        // Widening the quantized type to avoid overflow.
        Halide::Var index;
        Halide::Func widen;
        widen(index) = Halide::cast<int>(vec[0](index));
        widen.bound(index, 0, matrixRowDimension);

        auto quantized = VectorByMatrixTimes(widen, matrix[0], matrixRowDimension, matrixColumnDimension);

        Halide::Func result("VectorByMatrixTimesQuantized");
        Halide::Var matrixColumnIndex("matrixColumnIndex");

        result(matrixColumnIndex) = Halide::print(quantized(matrixColumnIndex), "Quantized") * Halide::print(vec[1](), "VecStep") * Halide::print(matrix[1](), "MatStep");
        result.bound(matrixColumnIndex, 0, matrixColumnDimension);
        return result;
    }

    template<class Type, class QuantizedType>
    inline std::vector<Halide::Func> Quantize(
        Halide::Func vector,
        int vectorRowDimension,
        int numReservedBits)
    {
        Halide::Func minMaxHead("minMaxPartial");
        Halide::Var subRowIndex("subRowIndex");
        minMaxHead(subRowIndex) = { std::numeric_limits<Type>::max(), std::numeric_limits<Type>::min() };

        Halide::RDom k1(0, vectorRowDimension / c_VectorizationWidth, "vectorizedDom");
        Halide::Expr inputValue = vector(subRowIndex + (k1 * c_VectorizationWidth));
        minMaxHead(subRowIndex) = { Halide::min(minMaxHead(subRowIndex)[0], inputValue), Halide::max(minMaxHead(subRowIndex)[1], inputValue) };

        Halide::RDom k2((vectorRowDimension / c_VectorizationWidth) * c_VectorizationWidth, vectorRowDimension % c_VectorizationWidth);
        Halide::Func minMaxTail("minMaxTail");
        minMaxTail() = { Halide::minimum(vector(k2)), Halide::maximum(vector(k2)) };

        Halide::Func minMax("minMax");
        Halide::RDom k3(0, c_VectorizationWidth);
        minMax() =
        {
            Halide::min(Halide::minimum(minMaxHead(k3)[0]), minMaxTail()[0]),
            Halide::max(Halide::maximum(minMaxHead(k3)[1]), minMaxTail()[1])
        };

        Halide::Func absMax("absMax");
        absMax() = print(Halide::max(-minMax()[0], minMax()[1]) * (1 << numReservedBits), "max");

        // Quantize, same procedure as in MLP library
        const int numQuantizedTypeBits = sizeof(QuantizedType) * 8;

        // We still need one bit for representing the sign, that's why - 1.
        auto quantizedTypeMaxValue = std::numeric_limits<QuantizedType>::max();

        Halide::Func qStep("qstep");
        qStep() = print(absMax() / (quantizedTypeMaxValue + 0.5f), "qstep"); // 0.5 is for rounding.

        Halide::Func inverseQStep("inverseqstep");
        inverseQStep() = (quantizedTypeMaxValue + 0.5f) / absMax();

        Halide::Func quantized("quantized");
        Halide::Var index("quantizedIndex");
        // + 1 for the edge case of quantizing -quantizedTypeMaxValue and 0.5 for rounding.
        quantized(index) = Halide::cast(Halide::type_of<QuantizedType>(), Halide::cast(Halide::Int(32), (vector(index) * inverseQStep() + quantizedTypeMaxValue + 1.5f) - (1 + quantizedTypeMaxValue)));

        // Schedule
        minMaxHead.bound(subRowIndex, 0, c_VectorizationWidth);
        minMaxHead.vectorize(subRowIndex, c_VectorizationWidth);
        minMaxHead.compute_root().update().vectorize(subRowIndex, c_VectorizationWidth);
        qStep.compute_root();
        inverseQStep.compute_root();
        return std::vector<Halide::Func>{ quantized, qStep };
    }

    // Offline quantization, please use only for non perf critical tasks,
    // i.e. quantization of parameters.
    template<class OriginalType, class QuantizedType>
    inline std::pair<std::vector<QuantizedType>, OriginalType> Quantize(std::vector<OriginalType> value, int numReservedBits)
    {
        int size = (int)value.size();
        Halide::Buffer<OriginalType> b(value.data(), size);
        Halide::Func w;
        Halide::Var index;
        w(index) = b(index);
        w.bound(index, 0, (int)value.size());

        auto quantize = Halide::Pipeline(Quantize<OriginalType, QuantizedType>(w, size, numReservedBits));

        std::vector<QuantizedType> result;
        result.resize(value.size());
        Halide::Buffer<QuantizedType> quantized(result.data(), size);
        Halide::Buffer<OriginalType> step = Halide::Buffer<OriginalType>::make_scalar("step");
        quantize.realize({ quantized, step });

        return std::make_pair(result, step(0));
    }

    // Actually for speech models there is no need in using this function, because vectors are the only
    // quantized entities at runtime.
    template<class OriginalType, class QuantizedType>
    inline std::vector<Halide::Func> Quantize(Halide::Func matrix,
        int matrixRowDimension,
        int matrixColumnDimension,
        int numReservedBits)
    {
        // Flatten
        Halide::Var index("index");
        Halide::Func asVector("asVector");
        asVector(index) = matrix(index / matrixRowDimension, index % matrixRowDimension);
        asVector.bound(index, 0, matrixRowDimension * matrixColumnDimension);

        auto result = Quantize<OriginalType, QuantizedType>(
            asVector,
            matrixRowDimension * matrixColumnDimension,
            numReservedBits);

        // Unflatten
        Halide::Var x, y;
        Halide::Func quantizedMatrix("quantizedMatrix");
        quantizedMatrix(x, y) = result[0](x * matrixRowDimension + y);
        quantizedMatrix.bound(x, 0, matrixColumnDimension);
        quantizedMatrix.bound(y, 0, matrixRowDimension);
        return { quantizedMatrix, result[1] };
    }
}