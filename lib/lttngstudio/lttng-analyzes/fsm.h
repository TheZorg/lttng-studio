#ifndef FSM_H
#define FSM_H

template<unsigned... Is> struct seq { };
template<unsigned N, unsigned... Is>
struct seq_generator : seq_generator<N-1, N-1, Is...> { };
template<unsigned... Is>
struct seq_generator<0, Is...> : seq<Is...> { };

struct BaseEventParams { virtual ~BaseEventParams(){} };

template<typename StateType, typename EventType, typename DataType, typename StateMachineType>
class BaseStateMachine {
protected:
    typedef StateType States;
    typedef EventType Events;
    typedef DataType Data;

public:
    DataType data;
    template<EventType E>
    struct EventParams : public BaseEventParams {};

private:
    typedef std::array<StateType, static_cast<unsigned>(StateType::SIZE)> StateArray;
    typedef std::array<StateArray, static_cast<unsigned>(EventType::SIZE)> TransitionTable;

    typedef void (StateMachineType::*TransitionFunction)(const BaseEventParams &params);
    typedef std::array<TransitionFunction, static_cast<unsigned>(StateType::SIZE)> TransitionFunctionArray;
    typedef std::array<TransitionFunctionArray, static_cast<unsigned>(EventType::SIZE)> TransitionFunctionTable;

    template<StateType S, EventType E>
    void doTransitionFunction(const BaseEventParams &params) {
        const EventParams<E> &p = dynamic_cast<const EventParams<E>&>(params);
        static_cast<StateMachineType*>(this)->template doTransitionFunctionSpec<S, E>(p);
    }

    template<EventType E, unsigned... Is>
    static constexpr StateArray makeArrayForEvent(seq<Is...>) {
        return { { StateMachineType::template getTransition<StateType(Is), E>()... } };
    }

    template<EventType E>
    static constexpr StateArray makeArrayForEvent() {
        return makeArrayForEvent<E>(seq_generator<static_cast<unsigned>(StateType::SIZE)>{});
    }

    template<unsigned... Is>
    static constexpr TransitionTable makeTransitions(seq<Is...>) {
        return { { makeArrayForEvent<static_cast<EventType>(Is)>()... } };
    }

    static constexpr TransitionTable makeTransitions() {
        return makeTransitions(seq_generator<static_cast<unsigned>(EventType::SIZE)>{});
    }

    template<EventType E, unsigned... Is>
    static constexpr TransitionFunctionArray makeTransitionFunctionsForEvent(seq<Is...>) {
        return {{ (TransitionFunction)&StateMachineType::template doTransitionFunction<static_cast<StateType>(Is), E>... }};
    }

    template<EventType E>
    static constexpr TransitionFunctionArray makeTransitionFunctionsForEvent() {
        return makeTransitionFunctionsForEvent<E>(seq_generator<static_cast<unsigned>(StateType::SIZE)>{});
    }

    template<unsigned... Is>
    static constexpr TransitionFunctionTable makeTransitionFunctions(seq<Is...>) {
        return { { makeTransitionFunctionsForEvent<static_cast<EventType>(Is)>()... } };
    }

    static constexpr TransitionFunctionTable makeTransitionFunctions() {
        return makeTransitionFunctions(seq_generator<static_cast<unsigned>(EventType::SIZE)>{});
    }

    template<unsigned... Is>
    static constexpr StateArray makeStates(seq<Is...>) {
        return { { StateType(Is)... } };
    }

    static constexpr StateArray makeStates() {
        return makeStates(seq_generator<static_cast<unsigned>(StateType::SIZE)>{});
    }

protected:
    static constexpr TransitionTable transitions = makeTransitions();
    static constexpr TransitionFunctionTable transitionFunctions = makeTransitionFunctions();
    StateArray enumerativeCurrentStates = makeStates();
    StateType currentState = static_cast<StateType>(0);
    bool isEnumerativePass = true;

public:
    void setEnumerativePass(bool p) {
        isEnumerativePass = p;
    }

    void doEnumerativeTransition(EventType event) {
        for (int i = 0; i < enumerativeCurrentStates.size(); i++) {
            enumerativeCurrentStates[i] = transitions[static_cast<int>(event)][static_cast<int>(enumerativeCurrentStates[i])];
        }
    }

    void setCurrentState(StateType state) {
        currentState = state;
    }

    template<typename ParamsType>
    void doTransition(EventType event, const ParamsType &params) {
        if (isEnumerativePass) {
            doEnumerativeTransition(event);
        } else {
            TransitionFunction f = transitionFunctions[static_cast<int>(event)][static_cast<int>(currentState)];
            (static_cast<StateMachineType*>(this)->*f)(static_cast<const BaseEventParams&>(params));
            currentState = transitions[static_cast<int>(event)][static_cast<int>(currentState)];
        }
    }

    StateType getCurrentStateForInitialState(StateType initialState) {
        return enumerativeCurrentStates[static_cast<int>(initialState)];
    }

    StateType getCurrentState() {
        return currentState;
    }
};
template<typename StateType, typename EventType, typename DataType, typename StateMachineType>
constexpr typename BaseStateMachine<StateType, EventType, DataType, StateMachineType>::TransitionTable BaseStateMachine<StateType, EventType, DataType, StateMachineType>::transitions;
template<typename StateType, typename EventType, typename DataType, typename StateMachineType>
constexpr typename BaseStateMachine<StateType, EventType, DataType, StateMachineType>::TransitionFunctionTable BaseStateMachine<StateType, EventType, DataType, StateMachineType>::transitionFunctions;

#define DEFAULT_TRANSITIONS \
    template<BaseStateMachine::States S, BaseStateMachine::Events E> \
    static constexpr BaseStateMachine::States getTransition() { return S; }

#define DEF_TRANSITION(stateMachine, startState, event, endState) \
    template<> \
    constexpr stateMachine::BaseStateMachine::States stateMachine::getTransition<startState, event>() { \
        return endState; \
    }

#endif // FSM_H
