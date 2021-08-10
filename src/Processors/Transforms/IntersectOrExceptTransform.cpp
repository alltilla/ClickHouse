#include <Processors/Transforms/IntersectOrExceptTransform.h>


namespace DB
{

/*
 * There are always at least two inputs. Number of operators is always number of inputs minus 1.
 * input1 {operator1} input2 {operator2} input3 ...
**/
IntersectOrExceptTransform::IntersectOrExceptTransform(const Block & header_, const Operators & operators_)
    : IProcessor(InputPorts(operators_.size() + 1, header_), {header_})
    , operators(operators_)
    , first_input(inputs.begin())
    , second_input(std::next(inputs.begin()))
{
    const Names & columns = header_.getNames();
    size_t num_columns = columns.empty() ? header_.columns() : columns.size();

    key_columns_pos.reserve(columns.size());
    for (size_t i = 0; i < num_columns; ++i)
    {
        auto pos = columns.empty() ? i : header_.getPositionByName(columns[i]);
        key_columns_pos.emplace_back(pos);
    }
}


IntersectOrExceptTransform::Status IntersectOrExceptTransform::prepare()
{
    auto & output = outputs.front();

    if (output.isFinished())
    {
        for (auto & in : inputs)
            in.close();

        return Status::Finished;
    }

    if (!output.canPush())
    {
        for (auto & input : inputs)
            input.setNotNeeded();

        return Status::PortFull;
    }

    if (finished_second_input)
    {
        if (first_input->isFinished() || (use_accumulated_input && !current_input_chunk))
        {
            std::advance(second_input, 1);

            if (second_input == inputs.end())
            {
                if (current_output_chunk)
                {
                    output.push(std::move(current_output_chunk));
                }

                output.finish();
                return Status::Finished;
            }
            else
            {
                use_accumulated_input = true;
                data.reset();
                finished_second_input = false;
                ++current_operator_pos;
            }
        }
    }
    else if (second_input->isFinished())
    {
        finished_second_input = true;
    }

    if (!has_input)
    {
        if (finished_second_input && use_accumulated_input)
        {
            current_input_chunk = std::move(current_output_chunk);
        }
        else
        {
            InputPort & input = finished_second_input ? *first_input : *second_input;

            input.setNeeded();
            if (!input.hasData())
                return Status::NeedData;

            current_input_chunk = input.pull();
        }

        has_input = true;
    }

    return Status::Ready;
}


void IntersectOrExceptTransform::work()
{
    if (!finished_second_input)
    {
        accumulate(std::move(current_input_chunk));
    }
    else
    {
        filter(current_input_chunk);
        current_output_chunk = std::move(current_input_chunk);
    }

    has_input = false;
}


template <typename Method>
void IntersectOrExceptTransform::addToSet(Method & method, const ColumnRawPtrs & columns, size_t rows, SetVariants & variants) const
{
    typename Method::State state(columns, key_sizes, nullptr);

    for (size_t i = 0; i < rows; ++i)
        state.emplaceKey(method.data, i, variants.string_pool);
}


template <typename Method>
size_t IntersectOrExceptTransform::buildFilter(
    Method & method, const ColumnRawPtrs & columns, IColumn::Filter & filter, size_t rows, SetVariants & variants) const
{
    typename Method::State state(columns, key_sizes, nullptr);
    size_t new_rows_num = 0;

    for (size_t i = 0; i < rows; ++i)
    {
        auto find_result = state.findKey(method.data, i, variants.string_pool);
        filter[i] = operators[current_operator_pos] == ASTIntersectOrExcept::Operator::EXCEPT ? !find_result.isFound() : find_result.isFound();
        if (filter[i])
            ++new_rows_num;
    }
    return new_rows_num;
}


void IntersectOrExceptTransform::accumulate(Chunk chunk)
{
    auto num_rows = chunk.getNumRows();
    auto columns = chunk.detachColumns();

    ColumnRawPtrs column_ptrs;
    column_ptrs.reserve(key_columns_pos.size());

    for (auto pos : key_columns_pos)
        column_ptrs.emplace_back(columns[pos].get());

    if (!data)
        data.emplace();

    if (data->empty())
        data->init(SetVariants::chooseMethod(column_ptrs, key_sizes));

    auto & data_set = *data;
    switch (data->type)
    {
        case SetVariants::Type::EMPTY:
            break;
#define M(NAME) \
    case SetVariants::Type::NAME: \
        addToSet(*data_set.NAME, column_ptrs, num_rows, data_set); \
        break;
            APPLY_FOR_SET_VARIANTS(M)
#undef M
    }
}


void IntersectOrExceptTransform::filter(Chunk & chunk)
{
    auto num_rows = chunk.getNumRows();
    auto columns = chunk.detachColumns();

    ColumnRawPtrs column_ptrs;
    column_ptrs.reserve(key_columns_pos.size());

    for (auto pos : key_columns_pos)
        column_ptrs.emplace_back(columns[pos].get());

    if (!data)
        data.emplace();

    if (data->empty())
        data->init(SetVariants::chooseMethod(column_ptrs, key_sizes));

    IColumn::Filter filter(num_rows);

    size_t new_rows_num = 0;
    auto & data_set = *data;
    switch (data->type)
    {
        case SetVariants::Type::EMPTY:
            break;
#define M(NAME) \
    case SetVariants::Type::NAME: \
        new_rows_num = buildFilter(*data_set.NAME, column_ptrs, filter, num_rows, data_set); \
        break;
            APPLY_FOR_SET_VARIANTS(M)
#undef M
    }

    for (auto & column : columns)
        column = column->filter(filter, -1);

    chunk.setColumns(std::move(columns), new_rows_num);
}

}
